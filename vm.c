#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "chunk.h"
#include "common.h"
#include "compiler.h"
#include "debug.h"
#include "memory.h"
#include "object.h"
#include "table.h"
#include "value.h"
#include "vm.h"

VM vm;

static Value clockNative(int argCount, Value *args) {
  return NUMBER_VAL((double)clock() / CLOCKS_PER_SEC);
}

static void resetStack() {
  vm.stackTop = vm.stack;
  vm.frameCount = 0;
  vm.openUpvalues = NULL;
}

void push(Value value) {
  *vm.stackTop = value;
  vm.stackTop++;
}

Value pop() {
  vm.stackTop--;
  return *vm.stackTop;
}

static void runtimeError(const char *format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputs("\n", stderr);

  for (int i = vm.frameCount - 1; i >= 0; i--) {
    CallFrame *frame = &vm.frames[i];
    ObjFunction *function = frame->closure->function;
    size_t instruction = frame->ip - function->chunk.code - 1;
    fprintf(stderr, "[line %d] in ", function->chunk.lines[instruction]);
    if (function->name == NULL) {
      fprintf(stderr, "script\n");
    } else {
      fprintf(stderr, "%s()\n", function->name->chars);
    }
  }

  resetStack();
}

// Foreign Function Interface to expose different native methods
// to the language users
static void defineNative(const char *name, NativeFn function) {
  push(OBJ_VAL(copyString(name, (int)strlen(name))));
  push(OBJ_VAL(newNative(function)));
  tableSet(&vm.globals, AS_STRING(vm.stack[0]), vm.stack[1]);
  pop();
  pop();
}

static Value peek(int distance) { return vm.stackTop[-1 - distance]; }

static bool call(ObjClosure *closure, int argCount) {
  if (argCount != closure->function->arity) {
    runtimeError("Expected %d number of arguments, but got %d",
                 closure->function->arity, argCount);
    return false;
  }

  if (vm.frameCount == FRAMES_MAX) {
    runtimeError("stack overflow.");
    return false;
  }

  CallFrame *frame = &vm.frames[vm.frameCount++];
  frame->closure = closure;
  frame->ip = closure->function->chunk.code;
  frame->slots = vm.stackTop - argCount - 1;
  return true;
}

static bool callValue(Value callee, int argCount) {
  if (IS_OBJ(callee)) {
    switch (OBJ_TYPE(callee)) {
    case OBJ_BOUND_METHOD: {
      ObjBoundMethod *bound = AS_BOUND_METHOD(callee);
      vm.stackTop[-argCount - 1] = bound->receiver;
      return call(bound->method, argCount);
    }
    case OBJ_CLASS: {
      /*
       * Instance are created Someclass(init1, init2..initn), similar to
       * function. Hence, we check if the Callee is of the OBJ_CLASS.
       *
       * Hence, when we parsed Someclass(), Someclass identifier is on the
       * stack, in that loc, we replace it with the SomeClass's instance.
       *
       */
      ObjClass *klass = AS_CLASS(callee);
      vm.stackTop[-argCount - 1] = OBJ_VAL(newInstance(klass));

      Value initializer;
      if (tableGet(&klass->methods, vm.initString, &initializer)) {
        return call(AS_CLOSURE(initializer), argCount);
      } else if (argCount != 0) {
        runtimeError("Expected 0 arguments but got %d.", argCount);
        return false;
      }

      return true;
    }
    case OBJ_CLOSURE: {
      return call(AS_CLOSURE(callee), argCount);
    }
    case OBJ_NATIVE: {
      // We don't need to manually setup CallFrame for native function.
      // Since, we don't have to generate INSTR. Native langauge will handle
      // the execution.
      NativeFn native = AS_NATIVE(callee);
      Value result = native(argCount, vm.stackTop - argCount);
      vm.stackTop -= argCount + 1;
      push(result);
      return true;
    }
    default:
      break; // non-callable
    }
  }
  runtimeError(" Can only call functions and classes.");
  return false;
}

static bool invokeFromClass(ObjClass *klass, ObjString *name, int argCount) {
  Value method;
  if (!tableGet(&klass->methods, name, &method)) {
    runtimeError("Undefined property '%s'", name->chars);
    return false;
  }

  return call(AS_CLOSURE(method), argCount);
}

static bool invoke(ObjString *name, int argCount) {
  Value receiver = peek(argCount);
  if (!IS_INSTANCE(receiver)) {
    runtimeError("only instances have methods.");
    return false;
  }
  ObjInstance *instance = AS_INSTANCE(receiver);

  Value value;
  if (tableGet(&instance->fields, name, &value)) {
    /*
     * This is generally how OP_GET_PROPERTY, then
     * OP_CALL behaves, we get the property, then
     * process the list of arguments. Followed by
     * OP_CALL which calls callValue.
     */
    vm.stackTop[-argCount - 1] = value;
    return callValue(value, argCount);
  }

  return invokeFromClass(instance->klass, name, argCount);
}

static bool bindMethod(ObjClass *klass, ObjString *name) {
  Value method;

  if (!tableGet(&klass->methods, name, &method)) {
    runtimeError("Undefined porperty '%s'.", name->chars);
    return false;
  }

  ObjBoundMethod *bound = newBoundMethod(peek(0), AS_CLOSURE(method));
  pop();
  push(OBJ_VAL(bound));
  return true;
}

static ObjUpvalue *captureUpvalue(Value *local) {
  ObjUpvalue *prevUpvalue = NULL;
  ObjUpvalue *upvalue = vm.openUpvalues;
  while (upvalue != NULL && upvalue->location > local) {
    prevUpvalue = upvalue;
    upvalue = upvalue->next;
  }

  // check if upvalue == NULL or we found the upvalue location
  if (upvalue != NULL && upvalue->location == local) {
    return upvalue;
  }

  ObjUpvalue *createdUpvalue = newUpvalue(local);
  // insert the upvalue to openUpvalues tracked by vm
  createdUpvalue->next = upvalue;

  if (prevUpvalue == NULL) {
    vm.openUpvalues = createdUpvalue;
  } else {
    prevUpvalue->next = createdUpvalue;
  }

  return createdUpvalue;
}

static void closeUpvalues(Value *last) {
  while (vm.openUpvalues != NULL && vm.openUpvalues->location >= last) {
    ObjUpvalue *upvalue = vm.openUpvalues;
    upvalue->closed = *upvalue->location;
    upvalue->location = &upvalue->closed;
    vm.openUpvalues = upvalue->next;
  }
}

static void defineMethod(ObjString *name) {
  Value method = peek(0);
  ObjClass *klass = AS_CLASS(peek(1));
  tableSet(&klass->methods, name, method);
  pop();
}

static bool isFalsey(Value value) {
  return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

static void concatenate() {
  ObjString *b = AS_STRING(peek(0));
  ObjString *a = AS_STRING(peek(1));
  int length = a->length + b->length;
  char *chars = ALLOCATE(char, length + 1);
  memcpy(chars, a->chars, a->length);
  memcpy(chars + a->length, b->chars, b->length);
  chars[length] = '\0';

  ObjString *result = takeString(chars, length);
  pop();
  pop();
  push(OBJ_VAL(result));
}

void initVM() {
  resetStack();
  vm.objects = NULL;
  vm.bytesAllocated = 0;
  vm.nextGC = 1024;

  initTable(&vm.globals);
  initTable(&vm.strings);
  vm.initString = NULL;
  vm.initString = copyString("init", 4);

  vm.grayCount = 0;
  vm.grayCapacity = 0;
  vm.grayStack = NULL;

  // Register the native functions using FFI: defineNative
  defineNative("clock", clockNative);
}

void freeVM() {
  freeTable(&vm.strings);
  vm.initString = NULL;
  freeTable(&vm.globals);
  freeObjects();
}

static InterpretResult run() {
  CallFrame *frame = &vm.frames[vm.frameCount - 1];

#define READ_BYTE() (*frame->ip++)
#define READ_CONSTANT()                                                        \
  (frame->closure->function->chunk.constants.values[READ_BYTE()])
#define READ_SHORT()                                                           \
  (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_STRING() AS_STRING(READ_CONSTANT())
#define BINARY_OP(valueType, op)                                               \
  do {                                                                         \
    if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) {                          \
      runtimeError("Operands must be numbers.");                               \
      return INTERPRET_RUNTIME_ERROR;                                          \
    }                                                                          \
    double b = AS_NUMBER(pop());                                               \
    double a = AS_NUMBER(pop());                                               \
    push(valueType(a op b));                                                   \
  } while (false)

  for (;;) {
    uint8_t instruction;

#ifdef DEBUG_TRACE_EXECUTION
    printf("         ");
    for (Value *slot = vm.stack; slot < vm.stackTop; slot++) {
      printf("[ ");
      printValue(*slot);
      printf(" ]");
    }
    printf("\n");
    disassembleInstruction(
        &frame->closure->function->chunk,
        (int)(frame->ip - frame->closure->function->chunk.code));
#endif

    switch (instruction = READ_BYTE()) {
    case OP_CONSTANT: {
      Value constant = READ_CONSTANT();
      push(constant);
      break;
    }
    case OP_NIL:
      push(NIL_VAL);
      break;
    case OP_TRUE:
      push(BOOL_VAL(true));
      break;
    case OP_FALSE:
      push(BOOL_VAL(false));
      break;
    case OP_POP:
      pop();
      break;
    case OP_GET_LOCAL: {
      /*
      {
       var a = 10;
       var b = a;
       a get local should get value from slot and push to top of the
       stack as it get evaluated as part of varDeclaration expression()
       for b.
      }
      */
      uint8_t slot = READ_BYTE();
      push(frame->slots[slot]);
      break;
    }
    case OP_DEFINE_GLOBAL: {
      ObjString *name = READ_STRING();
      tableSet(&vm.globals, name, peek(0));
      pop();
      break;
    }
    case OP_SET_LOCAL: {
      uint8_t slot = READ_BYTE();
      frame->slots[slot] = peek(0);
      break;
    }
    case OP_SET_GLOBAL: {
      ObjString *name = READ_STRING();
      if (tableSet(&vm.globals, name, peek(0))) {
        // tableSet returns true if new key, hence
        // we delete the key value we added and report
        // runtime error if its  a new value
        tableDelete(&vm.globals, name);
        runtimeError("Undefined variable '%s'.", name->chars);
        return INTERPRET_RUNTIME_ERROR;
      }
      // Why not popping? Because since this is an assignment expression
      // It can nested in another expression for instance a = b = 1
      // we need that value on stack.
    } break;
    case OP_SET_PROPERTY: {
      if (!IS_INSTANCE(peek(1))) {
        runtimeError("Only Instances have properties");
        return INTERPRET_RUNTIME_ERROR;
      }

      ObjInstance *instance = AS_INSTANCE(peek(1));
      tableSet(&instance->fields, READ_STRING(), peek(0));
      Value value = pop();
      pop();
      // Set expression produces a value, hence pushed value to stack.
      // var a = someInstance.field = 16;
      // print someInstance.field = "value";
      push(value);
      break;
    }
    case OP_GET_PROPERTY: {
      if (!IS_INSTANCE(peek(0))) {
        runtimeError("Only Instances have properties");
        return INTERPRET_RUNTIME_ERROR;
      }

      ObjInstance *instance = AS_INSTANCE(peek(0));
      ObjString *name = READ_STRING();

      Value value;
      if (tableGet(&instance->fields, name, &value)) {
        pop(); // Instance
        push(value);
        break;
      }

      if (!bindMethod(instance->klass, name)) {
        return INTERPRET_RUNTIME_ERROR;
      }
      break;
    }
    case OP_GET_GLOBAL: {
      ObjString *name = READ_STRING();
      Value value;

      if (!tableGet(&vm.globals, name, &value)) {
        runtimeError("Undefined Variable '%s'.", name->chars);
        return INTERPRET_RUNTIME_ERROR;
      }
      push(value);
      break;
    }
    case OP_GET_SUPER: {
      ObjString *name = READ_STRING();
      ObjClass *superclass = AS_CLASS(pop());
      if (!bindMethod(superclass, name)) {
        return INTERPRET_RUNTIME_ERROR;
      }
      break;
    }
    case OP_EQUAL: {
      Value b = pop();
      Value a = pop();
      push(BOOL_VAL(valuesEqual(a, b)));
      break;
    }
    case OP_SET_UPVALUE: {
      uint8_t slot = READ_BYTE();
      *frame->closure->upvalues[slot]->location = peek(0);
      break;
    }
    case OP_GET_UPVALUE: {
      uint8_t slot = READ_BYTE();
      push(*frame->closure->upvalues[slot]->location);
      break;
    }
    case OP_GREATER:
      BINARY_OP(BOOL_VAL, >);
      break;
    case OP_LESS:
      BINARY_OP(BOOL_VAL, <);
      break;
    case OP_ADD: {
      if (IS_STRING(peek(0)) && IS_STRING(peek(1))) {
        concatenate();
      } else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
        double b = AS_NUMBER(pop());
        double a = AS_NUMBER(pop());
        push(NUMBER_VAL(a + b));
      } else {
        runtimeError("Operands must be either two numbers or two strings.");
        return INTERPRET_RUNTIME_ERROR;
      }
      break;
    }
    case OP_SUBTRACT:
      BINARY_OP(NUMBER_VAL, -);
      break;
    case OP_MULTIPLY:
      BINARY_OP(NUMBER_VAL, *);
      break;
    case OP_DIVIDE:
      BINARY_OP(NUMBER_VAL, /);
      break;
    case OP_NOT:
      // Unlike in OP_NEGATE where we check if its a number, here we dont.
      // Everything except boolean False and Nil is True.
      // Just like how in python int, not empty list is considered True.
      push(BOOL_VAL(isFalsey(pop())));
      break;
    case OP_NEGATE:
      if (!IS_NUMBER(peek(0))) {
        runtimeError("Operand must be a number.");
        return INTERPRET_RUNTIME_ERROR;
      }
      push(NUMBER_VAL(-AS_NUMBER(pop())));
      break;
    case OP_PRINT: {
      printValue(pop());
      printf("\n");
      break;
    }
    case OP_JUMP: {
      uint16_t offset = READ_SHORT();
      frame->ip += offset;
      break;
    }
    case OP_JUMP_IF_FALSE: {
      uint16_t offset = READ_SHORT();
      if (isFalsey(peek(0))) {
        frame->ip += offset;
      }
      break;
    }
    case OP_LOOP: {
      uint16_t offset = READ_SHORT();
      frame->ip -= offset;
      break;
    }
    case OP_CALL: {
      int argCount = READ_BYTE();
      if (!callValue(peek(argCount), argCount)) {
        return INTERPRET_RUNTIME_ERROR;
      }
      // CallValue if valid func obj, creates a call frame
      // This frame has the slot pointer, ip of the function chunk etc.
      // Since, we are calling that, we should start executing from that IP,
      // hence we set the latest frame that was setup to invoke
      frame = &vm.frames[vm.frameCount - 1];
      break;
    }
    case OP_INVOKE: {
      ObjString *method = READ_STRING();
      int argCount = READ_BYTE();
      if (!invoke(method, argCount)) {
        return INTERPRET_RUNTIME_ERROR;
      }
      frame = &vm.frames[vm.frameCount - 1];
      break;
    }
    case OP_SUPER_INVOKE: {
      ObjString *method = READ_STRING();
      int argCount = READ_BYTE();
      ObjClass *superclass = AS_CLASS(pop());
      if (!invokeFromClass(superclass, method, argCount)) {
        return INTERPRET_RUNTIME_ERROR;
      }
      frame = &vm.frames[vm.frameCount - 1];
      break;
    }
    case OP_CLOSURE: {
      ObjFunction *function = AS_FUNCTION(READ_CONSTANT());
      ObjClosure *closure = newClosure(function);
      push(OBJ_VAL(closure));

      for (int i = 0; i < closure->upvalueCount; i++) {
        uint8_t isLocal = READ_BYTE();
        uint8_t index = READ_BYTE();
        if (isLocal) {
          closure->upvalues[i] = captureUpvalue(frame->slots + index);
        } else {
          closure->upvalues[i] = frame->closure->upvalues[index];
        }
      }
      break;
    }
    case OP_CLASS: {
      push(OBJ_VAL(newClass(READ_STRING())));
      break;
    }
    case OP_METHOD: {
      defineMethod(READ_STRING());
      break;
    }
    case OP_INHERIT: {
      Value superclass = peek(1);
      if (!(IS_CLASS(superclass))) {
        runtimeError("Superclass must be a class.");
        return INTERPRET_RUNTIME_ERROR;
      }
      ObjClass *subclass = AS_CLASS(peek(0));
      tableAddAll(&AS_CLASS(superclass)->methods, &subclass->methods);
      pop(); // subclass
      break;
    }
    case OP_CLOSE_UPVALUE: {
      closeUpvalues(vm.stackTop - 1);
      pop();
      break;
    }
    case OP_RETURN: {
      Value result = pop();
      /*
       * Important (closures): Ideally endScope should add OP_POP or
       * OP_CLOSE_UPVALUE for each local variable it removes from the stack at
       * the end of the function. However, in functions we don't call endScope
       * since we are popping the entire callframe (using return). As a result,
       * we need to capture the parameters and local declared variables in the
       * outerscope where a function body is declared before popping the
       * callframe. Hence, we close all the openUpvalues from the starting of
       * the callframe.
       */
      closeUpvalues(frame->slots);

      vm.frameCount--;
      if (vm.frameCount == 0) {
        // script (high level) function added at the top of the stack initially.
        pop();
        return INTERPRET_OK;
      }

      vm.stackTop = frame->slots;
      push(result);
      frame = &vm.frames[vm.frameCount - 1];
      break;
    }
    }
  }

#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
#undef BINARY_OP
}

InterpretResult interpret(const char *source) {
  ObjFunction *function = compile(source);
  if (function == NULL) {
    return INTERPRET_COMPILER_ERROR;
  }

  // If no compiler error, push function to stack (hence the 0th slot in
  // compiler is market with empty string), and initialize the CallFrame
  push(OBJ_VAL(function));
  ObjClosure *closure = newClosure(function);
  pop();
  push(OBJ_VAL(closure));
  // Sets up the window frame for the top level function
  // Replace:
  // CallFrame *frame = &vm.frames[vm.frameCount++];
  // frame->function = function;
  // frame->ip = function->chunk.code;
  // frame->slots = vm.stack;
  call(closure, 0);

  return run();
}
