/* Copyright (c) 2008, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.util;

public class Collections {
  private Collections() { }

  public static void shuffle(List list, Random random) {
    Object[] array = toArray(list, new Object[list.size()]);
    for (int i = 0; i < array.length; ++i) {
      int j = random.nextInt(array.length);
      Object tmp = array[i];
      array[i] = array[j];
      array[j] = tmp;
    }
 
    list.clear();
    for (int i = 0; i < array.length; ++i) {
      list.add(array[i]);
    }
  }

  public static void shuffle(List list) {
    shuffle(list, new Random());
  }

  static <T> T[] toArray(Collection collection, T[] array) {
    Class c = array.getClass().getComponentType();

    if (array.length < collection.size()) {
      array = (T[]) java.lang.reflect.Array.newInstance(c, collection.size());
    }

    int i = 0;
    for (Object o: collection) {
      if (c.isInstance(o)) {
        array[i++] = (T) o;
      } else {
        throw new ArrayStoreException();
      }
    }

    return array;
  }

  static String toString(Collection c) {
    StringBuilder sb = new StringBuilder();
    sb.append("[");
    for (Iterator it = c.iterator(); it.hasNext();) {
      sb.append(it.next());
      if (it.hasNext()) {
        sb.append(",");
      }
    }
    sb.append("]");
    return sb.toString();
  }

  static class IteratorEnumeration<T> implements Enumeration<T> {
    private final Iterator<T> it;

    public IteratorEnumeration(Iterator<T> it) {
      this.it = it;
    }

    public T nextElement() {
      return it.next();
    }

    public boolean hasMoreElements() {
      return it.hasNext();
    }
  }

  static class SynchronizedCollection<T> implements Collection<T> {
    protected final Object lock;
    protected final Collection<T> collection;

    public SynchronizedCollection(Object lock, Collection<T> collection) {
      this.lock = lock;
      this.collection = collection;
    }

    public int size() {
      synchronized (lock) { return collection.size(); }
    }

    public boolean isEmpty() {
      return size() == 0;
    }

    public boolean contains(T e) {
      synchronized (lock) { return collection.contains(e); }
    }

    public boolean add(T e) {
      synchronized (lock) { return collection.add(e); }
    }

    public boolean addAll(Collection<? extends T> collection) {
      synchronized (lock) { return this.collection.addAll(collection); }
    }

    public boolean remove(T e) {
      synchronized (lock) { return collection.remove(e); }
    }

    public <T> T[] toArray(T[] array) {
      synchronized (lock) { return collection.toArray(array); }
    }

    public void clear() {
      synchronized (lock) { collection.clear(); }
    }

    public Iterator<T> iterator() {
      return new SynchronizedIterator(lock, collection.iterator());
    }
  }
  
  static class SynchronizedMap<K,V> implements Map<K,V> {
    protected final Object lock;
    protected final Map<K,V> map;

    SynchronizedMap(Map<K,V> map) {
      this.map = map;
      this.lock = this;
    }

    SynchronizedMap(Object lock, Map<K,V> map) {
      this.lock = lock;
      this.map = map;
    }
    
    public void clear() {
      synchronized (lock) { map.clear(); }
    }
    public boolean containsKey(K key) {
      synchronized (lock) { return map.containsKey(key); }
    }
    public boolean containsValue(V value) {
      synchronized (lock) { return map.containsValue(value); }
    }
    public Set<java.util.Map.Entry<K, V>> entrySet() {
      synchronized (lock) { return new SynchronizedSet<java.util.Map.Entry<K, V>>(lock, map.entrySet()); }
    }
    public V get(K key) {
      synchronized (lock) { return map.get(key); }
    }
    public boolean isEmpty() {
      synchronized (lock) { return map.isEmpty(); }
    }
    public Set<K> keySet() {
      synchronized (lock) { return new SynchronizedSet<K>(lock, map.keySet()); }
    }
    public V put(K key, V value) {
      synchronized (lock) { return map.put(key, value); }
    }
    public void putAll(Map<? extends K, ? extends V> elts) {
      synchronized (lock) { map.putAll(elts); }
    }
    public V remove(K key) {
      synchronized (lock) { return map.remove(key); }
    }
    public int size() {
      synchronized (lock) { return map.size(); }
    }
    public Collection<V> values() {
      synchronized (lock) { return new SynchronizedCollection<V>(lock, map.values()); }
    }
  }
  
  public static <K,V> Map<K,V> synchronizedMap(Map<K,V> map) {
    return new SynchronizedMap<K, V> (map); 
  }
  

  static class SynchronizedSet<T>
    extends SynchronizedCollection<T>
    implements Set<T>
  {
    public SynchronizedSet(Object lock, Set<T> set) {
      super(lock, set);
    }
  }

  static class SynchronizedIterator<T> implements Iterator<T> {
    private final Object lock;
    private final Iterator<T> it;

    public SynchronizedIterator(Object lock, Iterator<T> it) {
      this.lock = lock;
      this.it = it;
    }

    public T next() {
      synchronized (lock) { return it.next(); }
    }

    public boolean hasNext() {
      synchronized (lock) { return it.hasNext(); }
    }

    public void remove() {
      synchronized (lock) { it.remove(); }
    }
  }

  static class ArrayListIterator<T> implements ListIterator<T> {
    private final List<T> list;
    private boolean canRemove = false;
    private int index;

    public ArrayListIterator(List<T> list) {
      this(list, 0);
    }

    public ArrayListIterator(List<T> list, int index) {
      this.list = list;
      this.index = index - 1;
    }

    public boolean hasPrevious() {
      return index >= 0;
    }

    public T previous() {
      if (hasPrevious()) {
        canRemove = true;
        return list.get(index--);
      } else {
        throw new NoSuchElementException();
      }
    }

    public T next() {
      if (hasNext()) {
        canRemove = true;
        return list.get(++index);
      } else {
        throw new NoSuchElementException();
      }
    }

    public boolean hasNext() {
      return index + 1 < list.size();
    }

    public void remove() {
      if (canRemove) {
        canRemove = false;
        list.remove(index--);
      } else {
        throw new IllegalStateException();
      }
    }
  }

  static class UnmodifiableSet<T> implements Set<T> {
    Set<T> inner;

    UnmodifiableSet(Set<T> inner) {
      this.inner = inner;
    }
          
    public boolean add(T element) {
      throw new UnsupportedOperationException("not supported");
    }

    public boolean addAll(Collection<? extends T> collection) {
      throw new UnsupportedOperationException("not supported");
    }

    public void clear() {
      throw new UnsupportedOperationException("not supported");
    }

    public boolean contains(T element) {
      return inner.contains(element);
    }

    public boolean isEmpty() {
      return inner.isEmpty();
    }

    public Iterator<T> iterator() {
      return inner.iterator();
    }

    public boolean remove(T element) {
      throw new UnsupportedOperationException("not supported");
    }

    public int size() {
      return inner.size();
    }

    public <S> S[] toArray(S[] array) {
      return inner.toArray(array);
    }
          
  }
  
  public static <T> Set<T> unmodifiableSet(Set<T> hs) {
    return new UnmodifiableSet<T>(hs);
  }
}
