#ifndef clox_object_h
#define clox_object_h

#include "chunk.h"
#include "common.h"
#include "value.h"

#define OBJ_TYPE(value) (AS_OBJ(value)->type)

// When downcasting from Obj --> ObjString, we need to ensure the type is string
#define IS_STRING(value) isObjType(value, OBJ_STRING)
#define IS_FUNCTION(value) isObjType(value, OBJ_FUNCTION)

#define AS_FUNCTION(value) ((ObjFunction *)AS_OBJ(value))
#define AS_STRING(value) ((ObjString *)AS_OBJ(value))
#define AS_CSTRING(value) (((ObjString *)AS_OBJ(value))->chars)

typedef enum {
  OBJ_STRING,
  OBJ_FUNCTION,
} ObjType;

struct Obj {
  ObjType type;
  struct Obj *next;
};

typedef struct {
  Obj obj;
  int arity;
  Chunk chunk;
  ObjString *name;
} ObjFunction;

struct ObjString {
  Obj obj;
  int length;
  char *chars;
  uint32_t hash;
};

ObjFunction *newFunction();
ObjString *takeString(char *chars, int length);
ObjString *copyString(const char *chars, int length);
void printObject(Value value);
// Why not define function itself as a macro?
// As seen, the body uses "value" twice, and macro is expanded
// by argument expression every place parameter name appear in the body.
// Hence, each argument expression is evaluated multiple times.
// If we pass something which has a side effect say IS_STRING(pop())
// We will end up executing pop() twice.
static inline bool isObjType(Value value, ObjType type) {
  return IS_OBJ(value) && AS_OBJ(value)->type == type;
}

#endif
