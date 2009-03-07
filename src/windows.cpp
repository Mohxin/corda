/* Copyright (c) 2008, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#include "sys/stat.h"
#include "windows.h"
#include "sys/timeb.h"
#include "dirent.h"

#undef max
#undef min

#include "x86.h"
#include "system.h"

#define ACQUIRE(s, x) MutexResource MAKE_NAME(mutexResource_) (s, x)

using namespace vm;

namespace {

class MutexResource {
 public:
  MutexResource(System* s, HANDLE m): s(s), m(m) {
    int r UNUSED = WaitForSingleObject(m, INFINITE);
    assert(s, r == WAIT_OBJECT_0);
  }

  ~MutexResource() {
    bool success UNUSED = ReleaseMutex(m);
    assert(s, success);
  }

 private:
  System* s; 
  HANDLE m;
};

class MySystem;
MySystem* system;

LONG CALLBACK
handleException(LPEXCEPTION_POINTERS e);

DWORD WINAPI
run(void* r)
{
  static_cast<System::Runnable*>(r)->run();
  return 0;
}

const bool Verbose = false;

const unsigned Waiting = 1 << 0;
const unsigned Notified = 1 << 1;

class MySystem: public System {
 public:
  class Thread: public System::Thread {
   public:
    Thread(System* s, System::Runnable* r):
      s(s),
      r(r),
      next(0),
      flags(0)
    {
      mutex = CreateMutex(0, false, 0);
      assert(s, mutex);

      event = CreateEvent(0, true, false, 0);
      assert(s, event);
    }

    virtual void interrupt() {
      ACQUIRE(s, mutex);

      r->setInterrupted(true);

      if (flags & Waiting) {
        int r UNUSED = SetEvent(event);
        assert(s, r == 0);
      }
    }

    virtual void join() {
      int r UNUSED = WaitForSingleObject(thread, INFINITE);
      assert(s, r == WAIT_OBJECT_0);
    }

    virtual void dispose() {
      CloseHandle(event);
      CloseHandle(mutex);
      CloseHandle(thread);
      s->free(this);
    }

    HANDLE thread;
    HANDLE mutex;
    HANDLE event;
    System* s;
    System::Runnable* r;
    Thread* next;
    unsigned flags;
  };

  class Mutex: public System::Mutex {
   public:
    Mutex(System* s): s(s) {
      mutex = CreateMutex(0, false, 0);
      assert(s, mutex);
    }

    virtual void acquire() {
      int r UNUSED = WaitForSingleObject(mutex, INFINITE);
      assert(s, r == WAIT_OBJECT_0);
    }

    virtual void release() {
      bool success UNUSED = ReleaseMutex(mutex);
      assert(s, success);
    }

    virtual void dispose() {
      CloseHandle(mutex);
      s->free(this);
    }

    System* s;
    HANDLE mutex;
  };

  class Monitor: public System::Monitor {
   public:
    Monitor(System* s): s(s), owner_(0), first(0), last(0), depth(0) {
      mutex = CreateMutex(0, false, 0);
      assert(s, mutex);
    }

    virtual bool tryAcquire(System::Thread* context) {
      Thread* t = static_cast<Thread*>(context);
      assert(s, t);

      if (owner_ == t) {
        ++ depth;
        return true;
      } else {
        switch (WaitForSingleObject(mutex, 0)) {
        case WAIT_TIMEOUT:
          return false;

        case WAIT_OBJECT_0:
          owner_ = t;
          ++ depth;
          return true;

        default:
          sysAbort(s);
        }
      }
    }

    virtual void acquire(System::Thread* context) {
      Thread* t = static_cast<Thread*>(context);
      assert(s, t);

      if (owner_ != t) {
        int r UNUSED = WaitForSingleObject(mutex, INFINITE);
        assert(s, r == WAIT_OBJECT_0);
        owner_ = t;
      }
      ++ depth;
    }

    virtual void release(System::Thread* context) {
      Thread* t = static_cast<Thread*>(context);
      assert(s, t);

      if (owner_ == t) {
        if (-- depth == 0) {
          owner_ = 0;
          bool success UNUSED = ReleaseMutex(mutex);
          assert(s, success);
        }
      } else {
        sysAbort(s);
      }
    }

    void append(Thread* t) {
      if (last) {
        last->next = t;
        last = t;
      } else {
        first = last = t;
      }
    }

    void remove(Thread* t) {
      Thread* previous = 0;
      for (Thread* current = first; current;) {
        if (t == current) {
          if (current == first) {
            first = t->next;
          } else {
            previous->next = t->next;
          }

          if (current == last) {
            last = previous;
          }

          t->next = 0;

          break;
        } else {
          previous = current;
          current = current->next;
        }
      }
    }

    virtual bool wait(System::Thread* context, int64_t time) {
      Thread* t = static_cast<Thread*>(context);
      assert(s, t);

      if (owner_ == t) {
        // Initialized here to make gcc 4.2 a happy compiler
        bool interrupted = false;
        bool notified = false;
        unsigned depth = 0;

        int r UNUSED;

        { ACQUIRE(s, t->mutex);
      
          if (t->r->interrupted()) {
            t->r->setInterrupted(false);
            return true;
          }

          t->flags |= Waiting;

          append(t);

          depth = this->depth;
          this->depth = 0;
          owner_ = 0;

          bool success UNUSED = ReleaseMutex(mutex);
          assert(s, success);

          success = ResetEvent(t->event);
          assert(s, success);

          success = ReleaseMutex(t->mutex);
          assert(s, success);

          r = WaitForSingleObject(t->event, (time ? time : INFINITE));
          assert(s, r == WAIT_OBJECT_0 or r == WAIT_TIMEOUT);

          r = WaitForSingleObject(t->mutex, INFINITE);
          assert(s, r == WAIT_OBJECT_0);

          notified = ((t->flags & Notified) != 0);
        
          t->flags = 0;

          interrupted = t->r->interrupted();
          if (interrupted) {
            t->r->setInterrupted(false);
          }
        }

        r = WaitForSingleObject(mutex, INFINITE);
        assert(s, r == WAIT_OBJECT_0);

        if (not notified) {
          remove(t);
        }

        t->next = 0;

        owner_ = t;
        this->depth = depth;

        return interrupted;
      } else {
        sysAbort(s);
      }
    }

    void doNotify(Thread* t) {
      ACQUIRE(s, t->mutex);

      t->flags |= Notified;

      bool success UNUSED = SetEvent(t->event);
      assert(s, success);
    }

    virtual void notify(System::Thread* context) {
      Thread* t = static_cast<Thread*>(context);
      assert(s, t);

      if (owner_ == t) {
        if (first) {
          Thread* t = first;
          first = first->next;
          if (t == last) {
            last = 0;
          }

          doNotify(t);
        }
      } else {
        sysAbort(s);
      }
    }

    virtual void notifyAll(System::Thread* context) {
      Thread* t = static_cast<Thread*>(context);
      assert(s, t);

      if (owner_ == t) {
        for (Thread* t = first; t; t = t->next) {
          doNotify(t);
        }
        first = last = 0;
      } else {
        sysAbort(s);
      }
    }
    
    virtual System::Thread* owner() {
      return owner_;
    }

    virtual void dispose() {
      assert(s, owner_ == 0);
      CloseHandle(mutex);
      s->free(this);
    }

    System* s;
    HANDLE mutex;
    Thread* owner_;
    Thread* first;
    Thread* last;
    unsigned depth;
  };

  class Local: public System::Local {
   public:
    Local(System* s): s(s) {
      key = TlsAlloc();
      assert(s, key != TLS_OUT_OF_INDEXES);
    }

    virtual void* get() {
      return TlsGetValue(key);
    }

    virtual void set(void* p) {
      bool r UNUSED = TlsSetValue(key, p);
      assert(s, r);
    }

    virtual void dispose() {
      bool r UNUSED = TlsFree(key);
      assert(s, r);

      s->free(this);
    }

    System* s;
    unsigned key;
  };

  class Region: public System::Region {
   public:
    Region(System* system, uint8_t* start, size_t length, HANDLE mapping,
           HANDLE file):
      system(system),
      start_(start),
      length_(length),
      mapping(mapping),
      file(file)
    { }

    virtual const uint8_t* start() {
      return start_;
    }

    virtual size_t length() {
      return length_;
    }

    virtual void dispose() {
      if (start_) {
        if (start_) UnmapViewOfFile(start_);
        if (mapping) CloseHandle(mapping);
        if (file) CloseHandle(file);
      }
      system->free(this);
    }

    System* system;
    uint8_t* start_;
    size_t length_;
    HANDLE mapping;
    HANDLE file;
  };

  class Directory: public System::Directory {
   public:
    Directory(System* s, DIR* directory): s(s), directory(directory) { }

    virtual const char* next() {
      if (directory) {
        dirent* e = readdir(directory);
        if (e) {
          return e->d_name;
        }
      }
      return 0;
    }

    virtual void dispose() {
      if (directory) {
        closedir(directory);
      }
      s->free(this);
    }

    System* s;
    DIR* directory;
  };

  class Library: public System::Library {
   public:
    Library(System* s, HMODULE handle, const char* name, bool mapName):
      s(s),
      handle(handle),
      name_(name),
      mapName_(mapName),
      next_(0)
    { }

    virtual void* resolve(const char* function) {
      void* address;
      FARPROC p = GetProcAddress(handle, function);
      memcpy(&address, &p, BytesPerWord);
      return address;
    }

    virtual const char* name() {
      return name_;
    }

    virtual bool mapName() {
      return mapName_;
    }

    virtual System::Library* next() {
      return next_;
    }

    virtual void setNext(System::Library* lib) {
      next_ = lib;
    }

    virtual void disposeAll() {
      if (Verbose) {
        fprintf(stderr, "close %p\n", handle);
      }

      if (name_) {
        FreeLibrary(handle);
      }

      if (next_) {
        next_->disposeAll();
      }

      if (name_) {
        s->free(name_);
      }

      s->free(this);
    }

    System* s;
    HMODULE handle;
    const char* name_;
    bool mapName_;
    System::Library* next_;
  };

  MySystem(const char* crashDumpDirectory):
    segFaultHandler(0),
    oldSegFaultHandler(0),
    crashDumpDirectory(crashDumpDirectory)
  {
    expect(this, system == 0);
    system = this;

    mutex = CreateMutex(0, false, 0);
    assert(this, mutex);
  }

  virtual void* tryAllocate(unsigned sizeInBytes) {
    return malloc(sizeInBytes);
  }

  virtual void free(const void* p) {
    if (p) ::free(const_cast<void*>(p));
  }

  virtual void* tryAllocateExecutable(unsigned sizeInBytes) {
    assert(this, sizeInBytes % LikelyPageSizeInBytes == 0);

    return VirtualAlloc
      (0, sizeInBytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  }

  virtual void freeExecutable(const void* p, unsigned) {
    int r UNUSED = VirtualFree(const_cast<void*>(p), 0, MEM_RELEASE);
    assert(this, r);
  }

  virtual bool success(Status s) {
    return s == 0;
  }

  virtual Status attach(Runnable* r) {
    Thread* t = new (allocate(this, sizeof(Thread))) Thread(this, r);
    bool success UNUSED = DuplicateHandle
      (GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
       &(t->thread), 0, false, DUPLICATE_SAME_ACCESS);
    assert(this, success);
    r->attach(t);
    return 0;
  }

  virtual Status start(Runnable* r) {
    Thread* t = new (allocate(this, sizeof(Thread))) Thread(this, r);
    r->attach(t);
    DWORD id;
    t->thread = CreateThread(0, 0, run, r, 0, &id);
    assert(this, t->thread);
    return 0;
  }

  virtual Status make(System::Mutex** m) {
    *m = new (allocate(this, sizeof(Mutex))) Mutex(this);
    return 0;
  }

  virtual Status make(System::Monitor** m) {
    *m = new (allocate(this, sizeof(Monitor))) Monitor(this);
    return 0;
  }

  virtual Status make(System::Local** l) {
    *l = new (allocate(this, sizeof(Local))) Local(this);
    return 0;
  }

  virtual Status handleSegFault(SignalHandler* handler) {
    if (handler) {
      segFaultHandler = handler;

      oldSegFaultHandler = SetUnhandledExceptionFilter(handleException);
      return 0;
    } else if (segFaultHandler) {
      segFaultHandler = 0;
      SetUnhandledExceptionFilter(oldSegFaultHandler);
      return 0;
    } else {
      return 1;
    }
  }

  virtual Status visit(System::Thread* st UNUSED, System::Thread* sTarget,
                       ThreadVisitor* visitor)
  {
    assert(this, st != sTarget);

    Thread* target = static_cast<Thread*>(sTarget);

    ACQUIRE(this, mutex);

    int rv = SuspendThread(target->thread);
    expect(this, rv != -1);

    CONTEXT context;
    rv = GetThreadContext(target->thread, &context);
    expect(this, rv);

    visitor->visit(reinterpret_cast<void*>(context.Eip),
                   reinterpret_cast<void*>(context.Ebp),
                   reinterpret_cast<void*>(context.Esp));

    rv = ResumeThread(target->thread);
    expect(this, rv != -1);

    return 0;
  }

  virtual uint64_t call(void* function, uintptr_t* arguments, uint8_t* types,
                        unsigned count, unsigned size, unsigned returnType)
  {
    return dynamicCall(function, arguments, types, count, size, returnType);
  }

  virtual Status map(System::Region** region, const char* name) {
    Status status = 1;
    
    HANDLE file = CreateFile(name, FILE_READ_DATA, FILE_SHARE_READ, 0,
                             OPEN_EXISTING, 0, 0);
    if (file != INVALID_HANDLE_VALUE) {
      unsigned size = GetFileSize(file, 0);
      if (size != INVALID_FILE_SIZE) {
        HANDLE mapping = CreateFileMapping(file, 0, PAGE_READONLY, 0, size, 0);
        if (mapping) {
          void* data = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
          if (data) {
            *region = new (allocate(this, sizeof(Region)))
              Region(this, static_cast<uint8_t*>(data), size, file, mapping);
            status = 0;        
          }

          if (status) {
            CloseHandle(mapping);
          }
        }
      }

      if (status) {
        CloseHandle(file);
      }
    }
    
    return status;
  }

  virtual Status open(System::Directory** directory, const char* name) {
    Status status = 1;
    
    DIR* d = opendir(name);
    if (d) {
      *directory = new (allocate(this, sizeof(Directory))) Directory(this, d);
      status = 0;
    }
    
    return status;
  }

  virtual FileType identify(const char* name) {
    struct _stat s;
    int r = _stat(name, &s);
    if (r == 0) {
      if (S_ISREG(s.st_mode)) {
        return TypeFile;
      } else if (S_ISDIR(s.st_mode)) {
        return TypeDirectory;
      } else {
        return TypeUnknown;
      }
    } else {
      return TypeDoesNotExist;
    }
  }

  virtual Status load(System::Library** lib,
                      const char* name,
                      bool mapName)
  {
    HMODULE handle;
    unsigned nameLength = (name ? strlen(name) : 0);
    if (mapName and name) {
      unsigned size = sizeof(SO_PREFIX) + nameLength + sizeof(SO_SUFFIX);
      char buffer[size];
      snprintf(buffer, size, SO_PREFIX "%s" SO_SUFFIX, name);
      handle = LoadLibrary(buffer);
    } else if (name) {
      handle = LoadLibrary(name);
    } else {
      handle = GetModuleHandle(0);
    }
 
    if (handle) {
      if (Verbose) {
        fprintf(stderr, "open %s as %p\n", name, handle);
      }

      char* n;
      if (name) {
        n = static_cast<char*>(allocate(this, nameLength + 1));
        memcpy(n, name, nameLength + 1);
      } else {
        n = 0;
      }

      *lib = new (allocate(this, sizeof(Library)))
        Library(this, handle, n, mapName);

      return 0;
    } else {
      return 1;
    }
  }

  virtual char pathSeparator() {
    return ';';
  }

  virtual int64_t now() {
    static LARGE_INTEGER frequency;
    static LARGE_INTEGER time;
    static bool init = true;

    if (init) {
      QueryPerformanceFrequency(&frequency);

      if (frequency.QuadPart == 0) {
        return 0;      
      }

      init = false;
    }

    QueryPerformanceCounter(&time);
    return static_cast<int64_t>
      (((static_cast<double>(time.QuadPart)) * 1000.0) /
       (static_cast<double>(frequency.QuadPart)));
  }

  virtual void exit(int code) {
    ::exit(code);
  }

  virtual void abort() {
    // trigger an EXCEPTION_ACCESS_VIOLATION, which we will catch and
    // generate a debug dump for
    *static_cast<int*>(0) = 0;
  }

  virtual void dispose() {
    system = 0;
    CloseHandle(mutex);
    ::free(this);
  }

  HANDLE mutex;
  System::SignalHandler* segFaultHandler;
  LPTOP_LEVEL_EXCEPTION_FILTER oldSegFaultHandler;
  const char* crashDumpDirectory;
};

struct MINIDUMP_EXCEPTION_INFORMATION {
  DWORD thread;
  LPEXCEPTION_POINTERS exception;
  BOOL exceptionInCurrentAddressSpace;
};

struct MINIDUMP_USER_STREAM_INFORMATION;
struct MINIDUMP_CALLBACK_INFORMATION;

enum MINIDUMP_TYPE {
  MiniDumpNormal = 0
};

typedef BOOL (*MiniDumpWriteDumpType)
(HANDLE processHandle,
 DWORD processId,
 HANDLE file,
 MINIDUMP_TYPE type,
 const MINIDUMP_EXCEPTION_INFORMATION* exception,
 const MINIDUMP_USER_STREAM_INFORMATION* userStream,
 const MINIDUMP_CALLBACK_INFORMATION* callback);

void
dump(LPEXCEPTION_POINTERS e, const char* directory)
{
  HINSTANCE dbghelp = LoadLibrary("dbghelp.dll");

  if (dbghelp) {
    MiniDumpWriteDumpType MiniDumpWriteDump = reinterpret_cast
      <MiniDumpWriteDumpType>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));

    if (MiniDumpWriteDump) {
      char name[MAX_PATH];
      _timeb tb;
      _ftime(&tb);
      snprintf(name, MAX_PATH, "%s\\crash-%lld.mdmp", directory,
               (static_cast<int64_t>(tb.time) * 1000)
               + static_cast<int64_t>(tb.millitm));

      HANDLE file = CreateFile
        (name, FILE_WRITE_DATA, 0, 0, CREATE_ALWAYS, 0, 0);

      if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception
          = { GetCurrentThreadId(), e, true };

        MiniDumpWriteDump
          (GetCurrentProcess(),
           GetCurrentProcessId(),
           file,
           MiniDumpNormal,
           &exception,
           0,
           0);

        CloseHandle(file);
      }
    }

    FreeLibrary(dbghelp);
  }
}

LONG CALLBACK
handleException(LPEXCEPTION_POINTERS e)
{
  if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
    void* ip = reinterpret_cast<void*>(e->ContextRecord->Eip);
    void* base = reinterpret_cast<void*>(e->ContextRecord->Ebp);
    void* stack = reinterpret_cast<void*>(e->ContextRecord->Esp);
    void* thread = reinterpret_cast<void*>(e->ContextRecord->Ebx);

    bool jump = system->segFaultHandler->handleSignal
      (&ip, &base, &stack, &thread);

    e->ContextRecord->Eip = reinterpret_cast<DWORD>(ip);
    e->ContextRecord->Ebp = reinterpret_cast<DWORD>(base);
    e->ContextRecord->Esp = reinterpret_cast<DWORD>(stack);
    e->ContextRecord->Ebx = reinterpret_cast<DWORD>(thread);

    if (jump) {
      return EXCEPTION_CONTINUE_EXECUTION;
    }
  }

  if (system->crashDumpDirectory) {
    dump(e, system->crashDumpDirectory);
  }

  return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

namespace vm {

System*
makeSystem(const char* crashDumpDirectory)
{
  return new (malloc(sizeof(MySystem))) MySystem(crashDumpDirectory);
}

} // namespace vm
