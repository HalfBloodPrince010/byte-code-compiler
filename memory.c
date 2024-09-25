#include <stdlib.h>

#include "compiler.h"
#include "memory.h"
#include "object.h"
#include "table.h"
#include "value.h"
#include "vm.h"

#ifdef DEBUG_LOG_GC
#include "debug.h"
#include <stdio.h>
#endif

#define GC_HEAP_GROW_FACTOR 2

void *reallocate(void *pointer, size_t oldSize, size_t newSize) {
  vm.bytesAllocated += newSize - oldSize;

  if (newSize > oldSize) {
#ifdef DEBUG_STRESS_GC
    collectGarbage();
#endif
  }

  if (vm.bytesAllocated > vm.nextGC) {
    collectGarbage();
  }

  if (newSize == 0) {
    free(pointer);
    return NULL;
  }

  void *result = realloc(pointer, newSize);
  if (result == NULL)
    exit(1);
  return result;
}

void markObject(Obj *object) {
  if (object == NULL) {
    return;
  }
  if (object->isMarked) {
    // We dont want to repeatedly add same objects to worklist
    return;
  }
#ifdef DEBUG_LOG_GC
  printf("%p mark", (void *)object);
  printValue(OBJ_VAL(object));
  printf("\n");
#endif
  object->isMarked = true;

  if (vm.grayCapacity < vm.grayCount + 1) {
    vm.grayCapacity = GROW_CAPACITY(vm.grayCapacity);
    /*
     * Note, we are using the std lib realloc instead of the wrapper
     * "reallocate" This is because, GC don't manage the grayStack.
     * If while dynamically allocating the grayStack, we will end up
     * calling GC again.
     */
    vm.grayStack =
        (Obj **)realloc(vm.grayStack, sizeof(Obj *) * vm.grayCapacity);

    if (vm.grayStack == NULL) {
      exit(1);
    }
  }

  vm.grayStack[vm.grayCount++] = object;
}

void markValue(Value value) {
  if (IS_OBJ(value))
    markObject(AS_OBJ(value));
}

static void markArray(ValueArray *array) {
  for (int i = 0; i < array->count; i++) {
    markValue(array->values[i]);
  }
}

static void blackenObject(Obj *object) {
#ifdef DEBUG_LOG_GC
  printf("%p blacken", (void *)object);
  printValue(OBJ_VAL(object));
  printf("\n");
#endif

  switch (object->type) {
  case OBJ_INSTANCE: {
    ObjInstance *instance = (ObjInstance *)object;
    markObject((Obj *)instance->klass);
    markTable(&instance->fields);
    break;
  }
  case OBJ_CLASS: {
    ObjClass *klass = (ObjClass *)object;
    markObject((Obj *)klass->name);
    break;
  }
  case OBJ_CLOSURE: {
    ObjClosure *closure = (ObjClosure *)object;
    markObject((Obj *)closure->function);
    for (int i = 0; i < closure->upvalueCount; i++) {
      markObject((Obj *)closure->upvalues[i]);
    }
    break;
  }
  case OBJ_FUNCTION: {
    ObjFunction *function = (ObjFunction *)object;
    markObject((Obj *)function->name);
    markArray(&function->chunk.constants);
    break;
  }
  // Value not in stack, but moved to upvalue
  case OBJ_UPVALUE:
    markValue(((ObjUpvalue *)object)->closed);
    break;
  case OBJ_NATIVE:
  case OBJ_STRING:
    break;
  }
}

static void freeObject(Obj *object) {
#ifdef DEBUG_LOG_GC
  printf("%p free type %d\n", (void *)object, object->type);
#endif

  switch (object->type) {
  case OBJ_INSTANCE: {
    ObjInstance *instance = (ObjInstance *)object;
    freeTable(&instance->fields);
    FREE(ObjInstance, object);
    break;
  }
  case OBJ_CLASS: {
    FREE(ObjClass, object);
    break;
  }
  case OBJ_CLOSURE: {
    ObjClosure *closure = (ObjClosure *)object;
    FREE_ARRAY(ObjClosure *, closure->upvalues, closure->upvalueCount);
    FREE(ObjClosure, object);
    break;
  }
  case OBJ_UPVALUE: {
    FREE(ObjUpvalue, object);
    break;
  }
  case OBJ_FUNCTION: {
    ObjFunction *function = (ObjFunction *)object;
    freeChunk(&function->chunk);
    FREE(ObjFunction, object);
    break;
  }
  case OBJ_NATIVE: {
    FREE(ObjNative, object);
    break;
  }
  case OBJ_STRING: {
    ObjString *string = (ObjString *)object;
    // Free the Dynamic String array
    FREE_ARRAY(char, string->chars, string->length + 1);
    // Free the ObjString struct
    FREE(ObjString, object);
    break;
  }
  }
}

static void markRoots() {
  // Stack Values
  for (Value *slot = vm.stack; slot < vm.stackTop; slot++) {
    markValue(*slot);
  }

  // Closures
  for (int i = 0; i < vm.frameCount; i++) {
    markObject((Obj *)vm.frames[i].closure);
  }

  // Upvalues, there are open upvalues, already closed "upvalues"
  // objects in closure upvalue array are indirect references of
  // closure. They are not direct "roots"
  for (ObjUpvalue *upvalue = vm.openUpvalues; upvalue != NULL;
       upvalue = upvalue->next) {
    markObject((Obj *)upvalue);
  }

  // Global table
  markTable(&vm.globals);

  // Compiler related objects (function obj, and its enclosings)
  markCompilerRoots();
}

static void traceReferences() {
  while (vm.grayCount > 0) {
    Obj *object = vm.grayStack[--vm.grayCount];
    // mark the current object black, then traces other references, and adds it
    // to grayStack/worklist.
    blackenObject(object);
  }
}

static void sweep() {
  Obj *prev = NULL;
  Obj *object = vm.objects;
  while (object != NULL) {
    if (object->isMarked) {
      // make these obj white for the GC cycle
      object->isMarked = false;
      prev = object;
      object = object->next;
    } else {
      Obj *unreached = object;
      object = object->next;
      if (prev != NULL) {
        prev->next = object;
      } else {
        vm.objects = object;
      }

      freeObject(unreached);
    }
  }
}

void collectGarbage() {
#ifdef DEBUG_LOG_GC
  printf("-- gc begin\n");
  size_t before = vm.bytesAllocated;
#endif

  markRoots();
  traceReferences();
  /*
   * We didn't mark "interned strings" as "roots" specifically due to fact that
   * interned strings table contains all the string. Marking it as root, we
   * don't eliminate /remove any strings. If we don't, then all the string will
   * be cleared by GC Hence, we need to "create weak references", and this
   * happens between marking obj roots and sweeping them. Since we know the
   * marked string, we can scan the table and deleted the ones that are marked
   * false.
   */
  tableRemoveWhite(&vm.strings);
  sweep();

  vm.nextGC = vm.bytesAllocated * GC_HEAP_GROW_FACTOR;
#ifdef DEBUG_LOG_GC
  printf("-- gc end\n");
  printf("    collected %zu bytes (from %zu to %zu) next GC at %zu\n",
         before - vm.bytesAllocated, before, vm.bytesAllocated, vm.nextGC);
#endif
}

void freeObjects() {
  Obj *object = vm.objects;
  while (object != NULL) {
    Obj *next = object->next;
    freeObject(object);
    object = next;
  }

  free(vm.grayStack);
}
