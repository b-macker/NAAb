# string_utils.naab

## Functions

- [toUpper](#toUpper)
- [contains](#contains)
- [split](#split)

---

## toUpper(str)

String utility functions for text processing Convert string to uppercase

**Parameters:**
- `str` - String to convert

**Returns:** Uppercased string

*Defined in string_utils.naab at line 6*

---

## contains(haystack, needle)

Check if string contains substring

**Parameters:**
- `haystack` - String to search in
- `needle` - Substring to find

**Returns:** true if needle is in haystack

*Defined in string_utils.naab at line 15*

---

## split(str, delimiter)

Split string by delimiter

**Parameters:**
- `str` - String to split
- `delimiter` - Character or string to split on

**Returns:** Array of substrings

**Example:**
```naab
split("a,b,c", ",") # Returns ["a", "b", "c"]
```

*Defined in string_utils.naab at line 25*

---

