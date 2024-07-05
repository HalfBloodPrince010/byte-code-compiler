#include <stdio.h>
#include <string.h>

#include "memory.h"
#include "object.h"
#include "value.h"
#include "vm.h"

#define ALLOCATE_OBJ(type, objectType)                                         \
  (type *)allocateObject(sizeof(type), objectType)

static Obj *allocateObject(size_t size, ObjType type) {
  Obj *object = (Obj *)reallocate(NULL, 0, size);
  object->type = type;
  return object;
}

static ObjString *allocateString(char *chars, int length) {
  ObjString *string = ALLOCATE_OBJ(ObjString, OBJ_STRING);
  string->length = length;
  string->chars = chars;
  return string;
}

// copyString() functions assumes it cannot take ownership (because its a source
// string) and we don't want to modify the source. As a result it creates a new
// copy of the data, and then allocates the ObjString on the heap. For
// concatenation we use takeString(), and the dynamic array has already been
// allocated.
ObjString *takeString(char *chars, int length) {
  return allocateString(chars, length);
}

// Note, we are copying the string by allocating memory in Heap
// We are not using the same source string pointer because,
// when we add string manipulation function, we might end up
// modifying the source code, hence "copying" the string to new location.
ObjString *copyString(const char *chars, int length) {
  char *heapChars = ALLOCATE(char, length + 1);
  memcpy(heapChars, chars, length);
  heapChars[length] = '\0';
  return allocateString(heapChars, length);
}

void printObject(Value value) {
  switch (OBJ_TYPE(value)) {
  case OBJ_STRING:
    printf("%s", AS_CSTRING(value));
    break;
  }
}