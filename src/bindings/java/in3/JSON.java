package in3;

import java.util.*;
import java.math.*;

/**
 * internal helper tool to represent a JSON-Object.
 * 
 * Since the internal representation of JSON in incubed uses hashes instead of
 * name, the getter will creates these hashes.
 */
public class JSON {
    static {
        System.loadLibrary("in3");
    }

    private HashMap<Integer, Object> map = new HashMap<Integer, Object>();

    private static native int key(String name);

    JSON() {
    }

    /**
     * gets the property
     * 
     * @return the raw object.
     */
    public Object get(String prop /** the name of the property. */
    ) {
        return map.get(key(prop));
    }

    /**
     * adds values. This function will be called from the JNI-Iterface.
     * 
     * Internal use only!
     */
    public void put(int key/** the hash of the key */
    ,Object val /** the value object */
    ) {
        map.put(key, val);
    }

    /**
     * returns the property as long
     * 
     * @return the long value
     */
    public long getLong(String key /** the propertyName */
    ) {
        return asLong(get(key));
    }

    /**
     * returns the property as BigInteger
     * 
     * @return the BigInteger value
     */
    public BigInteger getBigInteger(String key/** the propertyName */
    ) {
        return asBigInteger(get(key));
    }

    /**
     * returns the property as StringArray
     * 
     * @return the array or null
     */
    public String[] getStringArray(String key/** the propertyName */
    ) {
        return asStringArray(get(key));
    }

    /**
     * returns the property as String or in case of a number as hexstring.
     * 
     * @return the hexstring
     */
    public String getString(String key/** the propertyName */
    ) {
        return asString(get(key));
    }

    public String[] asStringArray(Object o) {
        if (o == null)
            return null;
        if (o instanceof Object[]) {
            Object[] a = (Object[]) o;
            String[] s = new String[a.length];
            for (int i = 0; i < s.length; i++)
                s[i] = a[i] == null ? null : a[i].toString();
            return s;
        }
        return null;
    }

    public static BigInteger asBigInteger(Object o) {
        if (o == null)
            return BigInteger.valueOf(0);
        if (o instanceof String)
            return (((String) o).length() > 2 && o.toString().charAt(1) == 'x')
                    ? new BigInteger(o.toString().substring(2), 16)
                    : new BigInteger(o.toString(), 10);
        if (o instanceof Integer)
            return BigInteger.valueOf(((Integer) o).longValue());
        return BigInteger.valueOf(0);

    }

    public static long asLong(Object o) {
        if (o == null)
            return 0;
        if (o instanceof String)
            return (((String) o).length() > 2 && o.toString().charAt(1) == 'x')
                    ? Long.parseLong(o.toString().substring(2), 16)
                    : Long.parseLong(o.toString(), 10);
        if (o instanceof Integer)
            return ((Integer) o).longValue();
        return 0;
    }

    public static String asString(Object o) {
        if (o == null)
            return null;
        if (o instanceof Integer)
            return "0x" + Integer.toHexString((Integer) o);
        if (o instanceof Long)
            return "0x" + Long.toHexString((Long) o);
        if (o instanceof BigInteger)
            return "0x" + ((BigInteger) o).toString(16);
        return o.toString();
    }

    public String toString() {
        return "<json-object>";
    }

    public static String toJson(Object ob) {
        if (ob == null)
            return "null";
        if (ob instanceof String)
            return "\"" + ob + "\"";
        if (ob instanceof BigInteger)
            return "\"0x" + ((BigInteger) ob).toString(16) + "\"";
        if (ob instanceof Number)
            return ob.toString();
        if (ob instanceof Boolean)
            return ob.toString();
        if (ob instanceof Object[]) {
            Object[] a = (Object[]) ob;
            StringBuilder sb = new StringBuilder();
            sb.append("[");
            for (int i = 0; i < a.length; i++) {
                if (i > 0)
                    sb.append(",");
                sb.append(toJson(a[i]));
            }
            return sb.append("]").toString();
        }
        return ob.toString();
    }

    public static void appendKey(StringBuilder sb, String key, Object value) {
        sb.append("\"").append(key).append("\":").append(toJson(value)).append(",");
    }
}
