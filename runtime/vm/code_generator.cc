// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/code_generator.h"

#include "vm/assembler_macros.h"
#include "vm/ast.h"
#include "vm/code_patcher.h"
#include "vm/compiler.h"
#include "vm/dart_api_impl.h"
#include "vm/dart_entry.h"
#include "vm/debugger.h"
#include "vm/deopt_instructions.h"
#include "vm/exceptions.h"
#include "vm/intermediate_language.h"
#include "vm/object_store.h"
#include "vm/message.h"
#include "vm/message_handler.h"
#include "vm/parser.h"
#include "vm/resolver.h"
#include "vm/runtime_entry.h"
#include "vm/stack_frame.h"
#include "vm/symbols.h"
#include "vm/verifier.h"

namespace dart {

DEFINE_FLAG(bool, deoptimize_alot, false,
    "Deoptimizes all live frames when we are about to return to Dart code from"
    " native entries.");
DEFINE_FLAG(bool, inline_cache, true, "Enable inline caches");
DEFINE_FLAG(bool, trace_deoptimization, false, "Trace deoptimization");
DEFINE_FLAG(bool, trace_deoptimization_verbose, false,
    "Trace deoptimization verbose");
DEFINE_FLAG(bool, trace_ic, false, "Trace IC handling");
DEFINE_FLAG(bool, trace_ic_miss_in_optimized, false,
    "Trace IC miss in optimized code");
DEFINE_FLAG(bool, trace_patching, false, "Trace patching of code.");
DEFINE_FLAG(bool, trace_runtime_calls, false, "Trace runtime calls");
DEFINE_FLAG(int, optimization_counter_threshold, 2000,
    "Function's usage-counter value before it is optimized, -1 means never");
DECLARE_FLAG(bool, enable_type_checks);
DECLARE_FLAG(bool, trace_type_checks);
DECLARE_FLAG(bool, report_usage_count);
DECLARE_FLAG(int, deoptimization_counter_threshold);
DEFINE_FLAG(charp, optimization_filter, NULL, "Optimize only named function");
DEFINE_FLAG(bool, trace_failed_optimization_attempts, false,
    "Traces all failed optimization attempts");
DEFINE_FLAG(bool, trace_optimized_ic_calls, false,
    "Trace IC calls in optimized code.");
DEFINE_FLAG(int, reoptimization_counter_threshold, 2000,
    "Counter threshold before a function gets reoptimized.");
DEFINE_FLAG(int, max_subtype_cache_entries, 100,
    "Maximum number of subtype cache entries (number of checks cached).");


DEFINE_RUNTIME_ENTRY(TraceFunctionEntry, 1) {
  ASSERT(arguments.ArgCount() ==
      kTraceFunctionEntryRuntimeEntry.argument_count());
  const Function& function = Function::CheckedHandle(arguments.ArgAt(0));
  const String& function_name = String::Handle(function.name());
  const String& class_name =
      String::Handle(Class::Handle(function.Owner()).Name());
  OS::Print("> Entering '%s.%s'\n",
      class_name.ToCString(), function_name.ToCString());
}


DEFINE_RUNTIME_ENTRY(TraceFunctionExit, 1) {
  ASSERT(arguments.ArgCount() ==
      kTraceFunctionExitRuntimeEntry.argument_count());
  const Function& function = Function::CheckedHandle(arguments.ArgAt(0));
  const String& function_name = String::Handle(function.name());
  const String& class_name =
      String::Handle(Class::Handle(function.Owner()).Name());
  OS::Print("< Exiting '%s.%s'\n",
      class_name.ToCString(), function_name.ToCString());
}


// Allocation of a fixed length array of given element type.
// This runtime entry is never called for allocating a List of a generic type,
// because a prior run time call instantiates the element type if necessary.
// Arg0: array length.
// Arg1: array type arguments, i.e. vector of 1 type, the element type.
// Return value: newly allocated array of length arg0.
DEFINE_RUNTIME_ENTRY(AllocateArray, 2) {
  ASSERT(arguments.ArgCount() == kAllocateArrayRuntimeEntry.argument_count());
  const Smi& length = Smi::CheckedHandle(arguments.ArgAt(0));
  const Array& array = Array::Handle(Array::New(length.Value()));
  arguments.SetReturn(array);
  AbstractTypeArguments& element_type =
      AbstractTypeArguments::CheckedHandle(arguments.ArgAt(1));
  // An Array is raw or takes only one type argument.
  ASSERT(element_type.IsNull() ||
         ((element_type.Length() == 1) && element_type.IsInstantiated()));
  array.SetTypeArguments(element_type);  // May be null.
}


// Allocate a new object.
// Arg0: class of the object that needs to be allocated.
// Arg1: type arguments of the object that needs to be allocated.
// Arg2: type arguments of the instantiator or kNoInstantiator.
// Return value: newly allocated object.
DEFINE_RUNTIME_ENTRY(AllocateObject, 3) {
  ASSERT(arguments.ArgCount() == kAllocateObjectRuntimeEntry.argument_count());
  const Class& cls = Class::CheckedHandle(arguments.ArgAt(0));
  const Instance& instance = Instance::Handle(Instance::New(cls));
  arguments.SetReturn(instance);
  if (!cls.HasTypeArguments()) {
    // No type arguments required for a non-parameterized type.
    ASSERT(Instance::CheckedHandle(arguments.ArgAt(1)).IsNull());
    return;
  }
  AbstractTypeArguments& type_arguments =
      AbstractTypeArguments::CheckedHandle(arguments.ArgAt(1));
  ASSERT(type_arguments.IsNull() ||
         (type_arguments.Length() == cls.NumTypeArguments()));
  // If no instantiator is provided, set the type arguments and return.
  if (Object::Handle(arguments.ArgAt(2)).IsSmi()) {
    ASSERT(Smi::CheckedHandle(arguments.ArgAt(2)).Value() ==
           StubCode::kNoInstantiator);
    instance.SetTypeArguments(type_arguments);  // May be null.
    return;
  }
  ASSERT(!type_arguments.IsInstantiated());
  const AbstractTypeArguments& instantiator =
      AbstractTypeArguments::CheckedHandle(arguments.ArgAt(2));
  ASSERT(instantiator.IsNull() || instantiator.IsInstantiated());
  if (instantiator.IsNull()) {
    type_arguments =
        InstantiatedTypeArguments::New(type_arguments, instantiator);
  } else if (instantiator.IsTypeArguments()) {
    // Code inlined in the caller should have optimized the case where the
    // instantiator is a TypeArguments and can be used as type argument vector.
    ASSERT(!type_arguments.IsUninstantiatedIdentity() ||
           (instantiator.Length() != type_arguments.Length()));
    type_arguments =
        InstantiatedTypeArguments::New(type_arguments, instantiator);
  } else {
    // If possible, use the instantiator as the type argument vector.
    if (type_arguments.IsUninstantiatedIdentity() &&
        (instantiator.Length() == type_arguments.Length())) {
      type_arguments = instantiator.raw();
    } else {
      type_arguments =
          InstantiatedTypeArguments::New(type_arguments, instantiator);
    }
  }
  ASSERT(type_arguments.IsInstantiated());
  instance.SetTypeArguments(type_arguments);
}


// Helper returning the token position of the Dart caller.
static intptr_t GetCallerLocation() {
  DartFrameIterator iterator;
  StackFrame* caller_frame = iterator.NextFrame();
  ASSERT(caller_frame != NULL);
  const Code& code = Code::Handle(caller_frame->LookupDartCode());
  const PcDescriptors& descriptors =
      PcDescriptors::Handle(code.pc_descriptors());
  ASSERT(!descriptors.IsNull());
  for (int i = 0; i < descriptors.Length(); i++) {
    if (static_cast<uword>(descriptors.PC(i)) == caller_frame->pc()) {
      return descriptors.TokenPos(i);
    }
  }
  return -1;
}


// Allocate a new object of a generic type and check that the instantiated type
// arguments are within the declared bounds or throw a dynamic type error.
// Arg0: class of the object that needs to be allocated.
// Arg1: type arguments of the object that needs to be allocated.
// Arg2: type arguments of the instantiator or kNoInstantiator.
// Return value: newly allocated object.
DEFINE_RUNTIME_ENTRY(AllocateObjectWithBoundsCheck, 3) {
  ASSERT(FLAG_enable_type_checks);
  ASSERT(arguments.ArgCount() ==
         kAllocateObjectWithBoundsCheckRuntimeEntry.argument_count());
  const Class& cls = Class::CheckedHandle(arguments.ArgAt(0));
  const Instance& instance = Instance::Handle(Instance::New(cls));
  arguments.SetReturn(instance);
  ASSERT(cls.HasTypeArguments());
  AbstractTypeArguments& type_arguments =
      AbstractTypeArguments::CheckedHandle(arguments.ArgAt(1));
  ASSERT(type_arguments.IsNull() ||
         (type_arguments.Length() == cls.NumTypeArguments()));
  AbstractTypeArguments& bounds_instantiator = AbstractTypeArguments::Handle();
  if (Object::Handle(arguments.ArgAt(2)).IsSmi()) {
    ASSERT(Smi::CheckedHandle(arguments.ArgAt(2)).Value() ==
           StubCode::kNoInstantiator);
  } else {
    ASSERT(!type_arguments.IsInstantiated());
    const AbstractTypeArguments& instantiator =
        AbstractTypeArguments::CheckedHandle(arguments.ArgAt(2));
    ASSERT(instantiator.IsNull() || instantiator.IsInstantiated());
    if (instantiator.IsNull()) {
      type_arguments =
          InstantiatedTypeArguments::New(type_arguments, instantiator);
    } else if (instantiator.IsTypeArguments()) {
      // Code inlined in the caller should have optimized the case where the
      // instantiator is a TypeArguments and can be used as type argument
      // vector.
      ASSERT(!type_arguments.IsUninstantiatedIdentity() ||
             (instantiator.Length() != type_arguments.Length()));
      type_arguments =
          InstantiatedTypeArguments::New(type_arguments, instantiator);
    } else {
      // If possible, use the instantiator as the type argument vector.
      if (type_arguments.IsUninstantiatedIdentity() &&
          (instantiator.Length() == type_arguments.Length())) {
        type_arguments = instantiator.raw();
      } else {
        type_arguments =
            InstantiatedTypeArguments::New(type_arguments, instantiator);
      }
    }
    bounds_instantiator = instantiator.raw();
  }
  if (!type_arguments.IsNull()) {
    ASSERT(type_arguments.IsInstantiated());
    Error& malformed_error = Error::Handle();
    if (!type_arguments.IsWithinBoundsOf(cls,
                                         bounds_instantiator,
                                         &malformed_error)) {
      ASSERT(!malformed_error.IsNull());
      // Throw a dynamic type error.
      const intptr_t location = GetCallerLocation();
      String& malformed_error_message =  String::Handle(
          String::New(malformed_error.ToErrorCString()));
      const String& no_name = String::Handle(Symbols::Empty());
      Exceptions::CreateAndThrowTypeError(
          location, no_name, no_name, no_name, malformed_error_message);
      UNREACHABLE();
    }
  }
  instance.SetTypeArguments(type_arguments);
}


// Instantiate type arguments.
// Arg0: uninstantiated type arguments.
// Arg1: instantiator type arguments.
// Return value: instantiated type arguments.
DEFINE_RUNTIME_ENTRY(InstantiateTypeArguments, 2) {
  ASSERT(arguments.ArgCount() ==
         kInstantiateTypeArgumentsRuntimeEntry.argument_count());
  AbstractTypeArguments& type_arguments =
      AbstractTypeArguments::CheckedHandle(arguments.ArgAt(0));
  const AbstractTypeArguments& instantiator =
      AbstractTypeArguments::CheckedHandle(arguments.ArgAt(1));
  ASSERT(!type_arguments.IsNull() && !type_arguments.IsInstantiated());
  ASSERT(instantiator.IsNull() || instantiator.IsInstantiated());
  // Code inlined in the caller should have optimized the case where the
  // instantiator can be used as type argument vector.
  ASSERT(instantiator.IsNull() ||
         !type_arguments.IsUninstantiatedIdentity() ||
         !instantiator.IsTypeArguments() ||
         (instantiator.Length() != type_arguments.Length()));
  type_arguments = InstantiatedTypeArguments::New(type_arguments, instantiator);
  ASSERT(type_arguments.IsInstantiated());
  arguments.SetReturn(type_arguments);
}


// Allocate a new closure.
// The type argument vector of a closure is always the vector of type parameters
// of its signature class, i.e. an uninstantiated identity vector. Therefore,
// the instantiator type arguments can be used as the instantiated closure type
// arguments and is passed here as the type arguments.
// Arg0: local function.
// Arg1: type arguments of the closure (i.e. instantiator).
// Return value: newly allocated closure.
DEFINE_RUNTIME_ENTRY(AllocateClosure, 2) {
  ASSERT(arguments.ArgCount() == kAllocateClosureRuntimeEntry.argument_count());
  const Function& function = Function::CheckedHandle(arguments.ArgAt(0));
  ASSERT(function.IsClosureFunction() && !function.IsImplicitClosureFunction());
  const AbstractTypeArguments& type_arguments =
      AbstractTypeArguments::CheckedHandle(arguments.ArgAt(1));
  ASSERT(type_arguments.IsNull() || type_arguments.IsInstantiated());
  // The current context was saved in the Isolate structure when entering the
  // runtime.
  const Context& context = Context::Handle(isolate->top_context());
  ASSERT(!context.IsNull());
  const Instance& closure = Instance::Handle(Closure::New(function, context));
  Closure::SetTypeArguments(closure, type_arguments);
  arguments.SetReturn(closure);
}


// Allocate a new implicit static closure.
// Arg0: local function.
// Return value: newly allocated closure.
DEFINE_RUNTIME_ENTRY(AllocateImplicitStaticClosure, 1) {
  ASSERT(arguments.ArgCount() ==
         kAllocateImplicitStaticClosureRuntimeEntry.argument_count());
  ObjectStore* object_store = isolate->object_store();
  ASSERT(object_store != NULL);
  const Function& function = Function::CheckedHandle(arguments.ArgAt(0));
  ASSERT(!function.IsNull());
  ASSERT(function.IsImplicitStaticClosureFunction());
  const Context& context = Context::Handle(object_store->empty_context());
  arguments.SetReturn(Instance::Handle(Closure::New(function, context)));
}


// Allocate a new implicit instance closure.
// Arg0: local function.
// Arg1: receiver object.
// Arg2: type arguments of the closure.
// Return value: newly allocated closure.
DEFINE_RUNTIME_ENTRY(AllocateImplicitInstanceClosure, 3) {
  ASSERT(arguments.ArgCount() ==
         kAllocateImplicitInstanceClosureRuntimeEntry.argument_count());
  const Function& function = Function::CheckedHandle(arguments.ArgAt(0));
  ASSERT(function.IsImplicitInstanceClosureFunction());
  const Instance& receiver = Instance::CheckedHandle(arguments.ArgAt(1));
  const AbstractTypeArguments& type_arguments =
      AbstractTypeArguments::CheckedHandle(arguments.ArgAt(2));
  ASSERT(type_arguments.IsNull() || type_arguments.IsInstantiated());
  Context& context = Context::Handle();
  context = Context::New(1);
  context.SetAt(0, receiver);
  const Instance& closure = Instance::Handle(Closure::New(function, context));
  Closure::SetTypeArguments(closure, type_arguments);
  arguments.SetReturn(closure);
}


// Allocate a new context large enough to hold the given number of variables.
// Arg0: number of variables.
// Return value: newly allocated context.
DEFINE_RUNTIME_ENTRY(AllocateContext, 1) {
  ASSERT(arguments.ArgCount() == kAllocateContextRuntimeEntry.argument_count());
  const Smi& num_variables = Smi::CheckedHandle(arguments.ArgAt(0));
  arguments.SetReturn(Context::Handle(Context::New(num_variables.Value())));
}


// Make a copy of the given context, including the values of the captured
// variables.
// Arg0: the context to be cloned.
// Return value: newly allocated context.
DEFINE_RUNTIME_ENTRY(CloneContext, 1) {
  ASSERT(arguments.ArgCount() == kCloneContextRuntimeEntry.argument_count());
  const Context& ctx = Context::CheckedHandle(arguments.ArgAt(0));
  Context& cloned_ctx = Context::Handle(Context::New(ctx.num_variables()));
  cloned_ctx.set_parent(Context::Handle(ctx.parent()));
  for (int i = 0; i < ctx.num_variables(); i++) {
    cloned_ctx.SetAt(i, Instance::Handle(ctx.At(i)));
  }
  arguments.SetReturn(cloned_ctx);
}


// Helper routine for tracing a type check.
static void PrintTypeCheck(
    const char* message,
    const Instance& instance,
    const AbstractType& type,
    const AbstractTypeArguments& instantiator_type_arguments,
    const Bool& result) {
  DartFrameIterator iterator;
  StackFrame* caller_frame = iterator.NextFrame();
  ASSERT(caller_frame != NULL);

  const Type& instance_type = Type::Handle(instance.GetType());
  ASSERT(instance_type.IsInstantiated());
  if (type.IsInstantiated()) {
    OS::Print("%s: '%s' %"Pd" %s '%s' %"Pd" (pc: %#"Px").\n",
              message,
              String::Handle(instance_type.Name()).ToCString(),
              Class::Handle(instance_type.type_class()).id(),
              (result.raw() == Bool::True()) ? "is" : "is !",
              String::Handle(type.Name()).ToCString(),
              Class::Handle(type.type_class()).id(),
              caller_frame->pc());
  } else {
    // Instantiate type before printing.
    const AbstractType& instantiated_type =
        AbstractType::Handle(type.InstantiateFrom(instantiator_type_arguments));
    OS::Print("%s: '%s' %s '%s' instantiated from '%s' (pc: %#"Px").\n",
              message,
              String::Handle(instance_type.Name()).ToCString(),
              (result.raw() == Bool::True()) ? "is" : "is !",
              String::Handle(instantiated_type.Name()).ToCString(),
              String::Handle(type.Name()).ToCString(),
              caller_frame->pc());
  }
  const Function& function = Function::Handle(
      caller_frame->LookupDartFunction());
  OS::Print(" -> Function %s\n", function.ToFullyQualifiedCString());
}


// Converts InstantiatedTypeArguments to TypeArguments and stores it
// into the instance. The assembly code can handle only type arguments of
// class TypeArguments. Because of the overhead, do it only when needed.
// Return false if the optimization was aborted.
// Set type_arguments_replaced to true if they have changed.
static bool OptimizeTypeArguments(const Instance& instance,
                                  bool* type_arguments_replaced) {
  *type_arguments_replaced = false;
  const Class& type_class = Class::ZoneHandle(instance.clazz());
  if (!type_class.HasTypeArguments()) {
    return true;
  }
  AbstractTypeArguments& type_arguments =
      AbstractTypeArguments::Handle(instance.GetTypeArguments());
  if (type_arguments.IsNull()) {
    return true;
  }
  if (type_arguments.IsInstantiatedTypeArguments()) {
    do {
      const InstantiatedTypeArguments& instantiated_type_arguments =
          InstantiatedTypeArguments::Cast(type_arguments);
      const AbstractTypeArguments& uninstantiated =
          AbstractTypeArguments::Handle(
              instantiated_type_arguments.uninstantiated_type_arguments());
      const AbstractTypeArguments& instantiator =
          AbstractTypeArguments::Handle(
              instantiated_type_arguments.instantiator_type_arguments());
      type_arguments = uninstantiated.InstantiateFrom(instantiator);
    } while (type_arguments.IsInstantiatedTypeArguments());
    AbstractTypeArguments& new_type_arguments = AbstractTypeArguments::Handle();
    new_type_arguments = type_arguments.Canonicalize();
    instance.SetTypeArguments(new_type_arguments);
    *type_arguments_replaced = true;
  } else if (!type_arguments.IsCanonical()) {
    AbstractTypeArguments& new_type_arguments = AbstractTypeArguments::Handle();
    new_type_arguments = type_arguments.Canonicalize();
    instance.SetTypeArguments(new_type_arguments);
    *type_arguments_replaced = true;
  }
  ASSERT(AbstractTypeArguments::Handle(
      instance.GetTypeArguments()).IsTypeArguments());
  return true;
}


// This updates the type test cache, an array containing 4-value elements
// (instance class, instance type arguments, instantiator type arguments and
// test_result). It can be applied to classes with type arguments in which
// case it contains just the result of the class subtype test, not including
// the evaluation of type arguments.
// This operation is currently very slow (lookup of code is not efficient yet).
// 'instantiator' can be null, in which case inst_targ
static void UpdateTypeTestCache(
    const Instance& instance,
    const AbstractType& type,
    const Instance& instantiator,
    const AbstractTypeArguments& incoming_instantiator_type_arguments,
    const Bool& result,
    const SubtypeTestCache& new_cache) {
  // Since the test is expensive, don't do it unless necessary.
  // The list of disallowed cases will decrease as they are implemented in
  // inlined assembly.
  if (new_cache.IsNull()) return;
  // Instantiator type arguments may be canonicalized later.
  AbstractTypeArguments& instantiator_type_arguments =
      AbstractTypeArguments::Handle(incoming_instantiator_type_arguments.raw());
  AbstractTypeArguments& instance_type_arguments =
      AbstractTypeArguments::Handle();
  const Class& instance_class = Class::Handle(instance.clazz());

  // Canonicalize type arguments.
  bool type_arguments_replaced = false;
  if (instance_class.HasTypeArguments()) {
    // Canonicalize type arguments.
    if (!OptimizeTypeArguments(instance, &type_arguments_replaced)) {
      if (FLAG_trace_type_checks) {
        PrintTypeCheck("WARNING: Cannot canonicalize instance type arguments",
            instance, type, instantiator_type_arguments, result);
      }
      return;
    }
    instance_type_arguments = instance.GetTypeArguments();
  }
  if (!instantiator.IsNull()) {
    bool replaced = false;
    if (!OptimizeTypeArguments(instantiator, &replaced)) {
      if (FLAG_trace_type_checks) {
        PrintTypeCheck("WARNING: Cannot canonicalize instantiator "
            "type arguments",
            instance, type, instantiator_type_arguments, result);
      }
      return;
    }
    if (replaced) {
      type_arguments_replaced = true;
    }
    instantiator_type_arguments = instantiator.GetTypeArguments();
  }

  intptr_t last_instance_class_id = -1;
  AbstractTypeArguments& last_instance_type_arguments =
      AbstractTypeArguments::Handle();
  AbstractTypeArguments& last_instantiator_type_arguments =
      AbstractTypeArguments::Handle();
  Bool& last_result = Bool::Handle();
  const intptr_t len = new_cache.NumberOfChecks();
  if (len >= FLAG_max_subtype_cache_entries) {
    return;
  }
  for (intptr_t i = 0; i < len; ++i) {
    new_cache.GetCheck(
        i,
        &last_instance_class_id,
        &last_instance_type_arguments,
        &last_instantiator_type_arguments,
        &last_result);
    if ((last_instance_class_id == instance_class.id()) &&
        (last_instance_type_arguments.raw() == instance_type_arguments.raw()) &&
        (last_instantiator_type_arguments.raw() ==
         instantiator_type_arguments.raw())) {
      if (FLAG_trace_type_checks) {
        OS::Print("%"Pd" ", i);
        if (type_arguments_replaced) {
          PrintTypeCheck("Duplicate cache entry (canonical.)", instance, type,
              instantiator_type_arguments, result);
        } else {
          PrintTypeCheck("WARNING Duplicate cache entry", instance, type,
              instantiator_type_arguments, result);
        }
      }
      // Can occur if we have canonicalized arguments.
      // TODO(srdjan): Investigate why this assert can fail.
      // ASSERT(type_arguments_replaced);
      return;
    }
  }
  if (!instantiator_type_arguments.IsInstantiatedTypeArguments()) {
    new_cache.AddCheck(instance_class.id(),
                       instance_type_arguments,
                       instantiator_type_arguments,
                       result);
  }
  if (FLAG_trace_type_checks) {
    AbstractType& test_type = AbstractType::Handle(type.raw());
    if (!test_type.IsInstantiated()) {
      test_type = type.InstantiateFrom(instantiator_type_arguments);
    }
    OS::Print("  Updated test cache %p ix: %"Pd" with (%"Pd", %p, %p, %s)\n"
        "    [%p %s %"Pd", %p %s]\n"
        "    [%p %s %"Pd", %p %s] %s\n",
        new_cache.raw(),
        len,
        instance_class.id(),

        instance_type_arguments.raw(),
        instantiator_type_arguments.raw(),
        result.ToCString(),

        instance_class.raw(),
        instance_class.ToCString(),
        instance_class.id(),
        instance_type_arguments.raw(),
        instance_type_arguments.ToCString(),

        test_type.type_class(),
        Class::Handle(test_type.type_class()).ToCString(),
        Class::Handle(test_type.type_class()).id(),
        instantiator_type_arguments.raw(),
        instantiator_type_arguments.ToCString(),
        result.ToCString());
  }
}


// Check that the given instance is an instance of the given type.
// Tested instance may not be null, because the null test is inlined.
// Arg0: instance being checked.
// Arg1: type.
// Arg2: instantiator (or null).
// Arg3: type arguments of the instantiator of the type.
// Arg4: SubtypeTestCache.
// Return value: true or false, or may throw a type error in checked mode.
DEFINE_RUNTIME_ENTRY(Instanceof, 5) {
  ASSERT(arguments.ArgCount() == kInstanceofRuntimeEntry.argument_count());
  const Instance& instance = Instance::CheckedHandle(arguments.ArgAt(0));
  const AbstractType& type = AbstractType::CheckedHandle(arguments.ArgAt(1));
  const Instance& instantiator = Instance::CheckedHandle(arguments.ArgAt(2));
  const AbstractTypeArguments& instantiator_type_arguments =
      AbstractTypeArguments::CheckedHandle(arguments.ArgAt(3));
  const SubtypeTestCache& cache =
      SubtypeTestCache::CheckedHandle(arguments.ArgAt(4));
  ASSERT(type.IsFinalized());
  Error& malformed_error = Error::Handle();
  const Bool& result = Bool::Handle(
      instance.IsInstanceOf(type,
                            instantiator_type_arguments,
                            &malformed_error) ?
      Bool::True() : Bool::False());
  if (FLAG_trace_type_checks) {
    PrintTypeCheck("InstanceOf",
        instance, type, instantiator_type_arguments, result);
  }
  if (!result.value() && !malformed_error.IsNull()) {
    // Throw a dynamic type error only if the instanceof test fails.
    const intptr_t location = GetCallerLocation();
    String& malformed_error_message =  String::Handle(
        String::New(malformed_error.ToErrorCString()));
    const String& no_name = String::Handle(Symbols::Empty());
    Exceptions::CreateAndThrowTypeError(
        location, no_name, no_name, no_name, malformed_error_message);
    UNREACHABLE();
  }
  UpdateTypeTestCache(instance, type, instantiator,
                      instantiator_type_arguments, result, cache);
  arguments.SetReturn(result);
}


// Check that the type of the given instance is a subtype of the given type and
// can therefore be assigned.
// Arg0: instance being assigned.
// Arg1: type being assigned to.
// Arg2: instantiator (or null).
// Arg3: type arguments of the instantiator of the type being assigned to.
// Arg4: name of variable being assigned to.
// Arg5: SubtypeTestCache.
// Return value: instance if a subtype, otherwise throw a TypeError.
DEFINE_RUNTIME_ENTRY(TypeCheck, 6) {
  ASSERT(arguments.ArgCount() == kTypeCheckRuntimeEntry.argument_count());
  const Instance& src_instance = Instance::CheckedHandle(arguments.ArgAt(0));
  const AbstractType& dst_type =
      AbstractType::CheckedHandle(arguments.ArgAt(1));
  const Instance& dst_instantiator =
      Instance::CheckedHandle(arguments.ArgAt(2));
  const AbstractTypeArguments& instantiator_type_arguments =
      AbstractTypeArguments::CheckedHandle(arguments.ArgAt(3));
  const String& dst_name = String::CheckedHandle(arguments.ArgAt(4));
  const SubtypeTestCache& cache =
      SubtypeTestCache::CheckedHandle(arguments.ArgAt(5));
  ASSERT(!dst_type.IsDynamicType());  // No need to check assignment.
  ASSERT(!dst_type.IsMalformed());  // Already checked in code generator.
  ASSERT(!src_instance.IsNull());  // Already checked in inlined code.

  Error& malformed_error = Error::Handle();
  const bool is_instance_of = src_instance.IsInstanceOf(
      dst_type, instantiator_type_arguments, &malformed_error);

  if (FLAG_trace_type_checks) {
    PrintTypeCheck("TypeCheck",
        src_instance, dst_type, instantiator_type_arguments,
        Bool::Handle(is_instance_of ? Bool::True() : Bool::False()));
  }
  if (!is_instance_of) {
    // Throw a dynamic type error.
    const intptr_t location = GetCallerLocation();
    const AbstractType& src_type = AbstractType::Handle(src_instance.GetType());
    const String& src_type_name = String::Handle(src_type.UserVisibleName());
    String& dst_type_name = String::Handle();
    if (!dst_type.IsInstantiated()) {
      // Instantiate dst_type before reporting the error.
      const AbstractType& instantiated_dst_type = AbstractType::Handle(
          dst_type.InstantiateFrom(instantiator_type_arguments));
      dst_type_name = instantiated_dst_type.UserVisibleName();
    } else {
      dst_type_name = dst_type.UserVisibleName();
    }
    String& malformed_error_message =  String::Handle();
    if (!malformed_error.IsNull()) {
      ASSERT(FLAG_enable_type_checks);
      malformed_error_message = String::New(malformed_error.ToErrorCString());
    }
    Exceptions::CreateAndThrowTypeError(location, src_type_name, dst_type_name,
                                        dst_name, malformed_error_message);
    UNREACHABLE();
  }
  UpdateTypeTestCache(src_instance, dst_type,
                      dst_instantiator, instantiator_type_arguments,
                      Bool::ZoneHandle(Bool::True()), cache);
  arguments.SetReturn(src_instance);
}


// Test whether a formal parameter was defined by a passed-in argument.
// Arg0: formal parameter index as Smi.
// Arg1: formal parameter name as Symbol.
// Arg2: arguments descriptor array.
// Return value: true or false.
DEFINE_RUNTIME_ENTRY(ArgumentDefinitionTest, 3) {
  ASSERT(arguments.ArgCount() ==
         kArgumentDefinitionTestRuntimeEntry.argument_count());
  const Smi& param_index = Smi::CheckedHandle(arguments.ArgAt(0));
  const String& param_name = String::CheckedHandle(arguments.ArgAt(1));
  ASSERT(param_name.IsSymbol());
  const Array& arg_desc = Array::CheckedHandle(arguments.ArgAt(2));
  const intptr_t num_pos_args = Smi::CheckedHandle(arg_desc.At(1)).Value();
  // Check if the formal parameter is defined by a positional argument.
  bool is_defined = num_pos_args > param_index.Value();
  if (!is_defined) {
    // Check if the formal parameter is defined by a named argument.
    const intptr_t num_named_args =
        Smi::CheckedHandle(arg_desc.At(0)).Value() - num_pos_args;
    String& arg_name = String::Handle();
    for (intptr_t i = 0; i < num_named_args; i++) {
      arg_name ^= arg_desc.At(2*i + 2);
      if (arg_name.raw() == param_name.raw()) {
        is_defined = true;
        break;
      }
    }
  }
  arguments.SetReturn(Bool::Handle(Bool::Get(is_defined)));
}


// Report that the type of the given object is not bool in conditional context.
// Arg0: bad object.
// Return value: none, throws a TypeError.
DEFINE_RUNTIME_ENTRY(ConditionTypeError, 1) {
  ASSERT(arguments.ArgCount() ==
      kConditionTypeErrorRuntimeEntry.argument_count());
  const intptr_t location = GetCallerLocation();
  const Instance& src_instance = Instance::CheckedHandle(arguments.ArgAt(0));
  ASSERT(src_instance.IsNull() || !src_instance.IsBool());
  const Type& bool_interface = Type::Handle(Type::BoolType());
  const AbstractType& src_type = AbstractType::Handle(src_instance.GetType());
  const String& src_type_name = String::Handle(src_type.UserVisibleName());
  const String& bool_type_name =
      String::Handle(bool_interface.UserVisibleName());
  const String& expr = String::Handle(Symbols::New("boolean expression"));
  const String& no_malformed_type_error = String::Handle();
  Exceptions::CreateAndThrowTypeError(location, src_type_name, bool_type_name,
                                      expr, no_malformed_type_error);
  UNREACHABLE();
}


// Report that the type of the type check is malformed.
// Arg0: src value.
// Arg1: name of instance being assigned to.
// Arg2: malformed type error message.
// Return value: none, throws an exception.
DEFINE_RUNTIME_ENTRY(MalformedTypeError, 3) {
  ASSERT(arguments.ArgCount() ==
      kMalformedTypeErrorRuntimeEntry.argument_count());
  const intptr_t location = GetCallerLocation();
  const Instance& src_value = Instance::CheckedHandle(arguments.ArgAt(0));
  const String& dst_name = String::CheckedHandle(arguments.ArgAt(1));
  const String& malformed_error = String::CheckedHandle(arguments.ArgAt(2));
  const String& dst_type_name = String::Handle(Symbols::New("malformed"));
  const AbstractType& src_type = AbstractType::Handle(src_value.GetType());
  const String& src_type_name = String::Handle(src_type.UserVisibleName());
  Exceptions::CreateAndThrowTypeError(location, src_type_name,
                                      dst_type_name, dst_name, malformed_error);
  UNREACHABLE();
}


DEFINE_RUNTIME_ENTRY(Throw, 1) {
  ASSERT(arguments.ArgCount() == kThrowRuntimeEntry.argument_count());
  const Instance& exception = Instance::CheckedHandle(arguments.ArgAt(0));
  Exceptions::Throw(exception);
}


DEFINE_RUNTIME_ENTRY(ReThrow, 2) {
  ASSERT(arguments.ArgCount() == kReThrowRuntimeEntry.argument_count());
  const Instance& exception = Instance::CheckedHandle(arguments.ArgAt(0));
  const Instance& stacktrace = Instance::CheckedHandle(arguments.ArgAt(1));
  Exceptions::ReThrow(exception, stacktrace);
}


// Patches static call with the target's entry point. Compiles target if
// necessary.
DEFINE_RUNTIME_ENTRY(PatchStaticCall, 0) {
  ASSERT(arguments.ArgCount() == kPatchStaticCallRuntimeEntry.argument_count());
  DartFrameIterator iterator;
  StackFrame* caller_frame = iterator.NextFrame();
  ASSERT(caller_frame != NULL);
  const Code& caller_code = Code::Handle(caller_frame->LookupDartCode());
  ASSERT(!caller_code.IsNull());
  const Function& target_function = Function::Handle(
      caller_code.GetStaticCallTargetFunctionAt(caller_frame->pc()));
  if (!target_function.HasCode()) {
    const Error& error =
        Error::Handle(Compiler::CompileFunction(target_function));
    if (!error.IsNull()) {
      Exceptions::PropagateError(error);
    }
  }
  const Code& target_code = Code::Handle(target_function.CurrentCode());
  // Before patching verify that we are not repeatedly patching to the same
  // target.
  ASSERT(target_code.EntryPoint() !=
         CodePatcher::GetStaticCallTargetAt(caller_frame->pc()));
  CodePatcher::PatchStaticCallAt(caller_frame->pc(), target_code.EntryPoint());
  caller_code.SetStaticCallTargetCodeAt(caller_frame->pc(), target_code);
  if (FLAG_trace_patching) {
    OS::Print("PatchStaticCall: patching from %#"Px" to '%s' %#"Px"\n",
        caller_frame->pc(),
        target_function.ToFullyQualifiedCString(),
        target_code.EntryPoint());
  }
  arguments.SetReturn(target_code);
}


// Resolves and compiles the target function of an instance call, updates
// function cache of the receiver's class and returns the compiled code or null.
// Only the number of named arguments is checked, but not the actual names.
RawCode* ResolveCompileInstanceCallTarget(Isolate* isolate,
                                          const Instance& receiver) {
  int num_arguments = -1;
  int num_named_arguments = -1;
  uword target = 0;
  String& function_name = String::Handle();
  DartFrameIterator iterator;
  StackFrame* caller_frame = iterator.NextFrame();
  ASSERT(caller_frame != NULL);
  CodePatcher::GetInstanceCallAt(caller_frame->pc(),
                                 &function_name,
                                 &num_arguments,
                                 &num_named_arguments,
                                 &target);
  ASSERT(function_name.IsSymbol());

  Function& function = Function::Handle();
  function = Resolver::ResolveDynamic(receiver,
                                      function_name,
                                      num_arguments,
                                      num_named_arguments);
  if (function.IsNull()) {
    return Code::null();
  } else {
    if (!function.HasCode()) {
      const Error& error = Error::Handle(Compiler::CompileFunction(function));
      if (!error.IsNull()) {
        Exceptions::PropagateError(error);
      }
    }
    return function.CurrentCode();
  }
}


// Result of an invoke may be an unhandled exception, in which case we
// rethrow it.
static void CheckResultError(const Object& result) {
  if (result.IsError()) {
    Exceptions::PropagateError(Error::Cast(result));
  }
}


// Resolves an instance function and compiles it if necessary.
//   Arg0: receiver object.
//   Returns: RawCode object or NULL (method not found or not compileable).
// This is called by the megamorphic stub when instance call does not need to be
// patched.
// Used by megamorphic lookup/no-such-method-handling.
DEFINE_RUNTIME_ENTRY(ResolveCompileInstanceFunction, 1) {
  ASSERT(arguments.ArgCount() ==
         kResolveCompileInstanceFunctionRuntimeEntry.argument_count());
  const Instance& receiver = Instance::CheckedHandle(arguments.ArgAt(0));
  const Code& code = Code::Handle(
      ResolveCompileInstanceCallTarget(isolate, receiver));
  arguments.SetReturn(code);
}


// Gets called from debug stub when code reaches a breakpoint.
DEFINE_RUNTIME_ENTRY(BreakpointStaticHandler, 0) {
  ASSERT(arguments.ArgCount() ==
      kBreakpointStaticHandlerRuntimeEntry.argument_count());
  ASSERT(isolate->debugger() != NULL);
  isolate->debugger()->SignalBpReached();
  // Make sure the static function that is about to be called is
  // compiled. The stub will jump to the entry point without any
  // further tests.
  DartFrameIterator iterator;
  StackFrame* caller_frame = iterator.NextFrame();
  ASSERT(caller_frame != NULL);
  const Code& code = Code::Handle(caller_frame->LookupDartCode());
  const Function& function =
      Function::Handle(code.GetStaticCallTargetFunctionAt(caller_frame->pc()));

  if (!function.HasCode()) {
    const Error& error = Error::Handle(Compiler::CompileFunction(function));
    if (!error.IsNull()) {
      Exceptions::PropagateError(error);
    }
  }
  arguments.SetReturn(Code::ZoneHandle(function.CurrentCode()));
}


// Gets called from debug stub when code reaches a breakpoint at a return
// in Dart code.
DEFINE_RUNTIME_ENTRY(BreakpointReturnHandler, 0) {
  ASSERT(arguments.ArgCount() ==
         kBreakpointReturnHandlerRuntimeEntry.argument_count());
  ASSERT(isolate->debugger() != NULL);
  isolate->debugger()->SignalBpReached();
}


// Gets called from debug stub when code reaches a breakpoint.
DEFINE_RUNTIME_ENTRY(BreakpointDynamicHandler, 0) {
  ASSERT(arguments.ArgCount() ==
     kBreakpointDynamicHandlerRuntimeEntry.argument_count());
  ASSERT(isolate->debugger() != NULL);
  isolate->debugger()->SignalBpReached();
}


static RawFunction* InlineCacheMissHandler(
    Isolate* isolate, const GrowableArray<const Instance*>& args) {
  const Instance& receiver = *args[0];
  const Code& target_code =
      Code::Handle(ResolveCompileInstanceCallTarget(isolate, receiver));
  if (target_code.IsNull()) {
    // Let the megamorphic stub handle special cases: NoSuchMethod,
    // closure calls.
    if (FLAG_trace_ic) {
      OS::Print("InlineCacheMissHandler NULL code for receiver: %s\n",
          receiver.ToCString());
    }
    return Function::null();
  }
  const Function& target_function =
      Function::Handle(target_code.function());
  ASSERT(!target_function.IsNull());
  DartFrameIterator iterator;
  StackFrame* caller_frame = iterator.NextFrame();
  ASSERT(caller_frame != NULL);
  ICData& ic_data = ICData::Handle(
      CodePatcher::GetInstanceCallIcDataAt(caller_frame->pc()));
  if (args.length() == 1) {
    ic_data.AddReceiverCheck(Class::Handle(args[0]->clazz()).id(),
                             target_function);
  } else {
    GrowableArray<intptr_t> class_ids(args.length());
    ASSERT(ic_data.num_args_tested() == args.length());
    for (intptr_t i = 0; i < args.length(); i++) {
      class_ids.Add(Class::Handle(args[i]->clazz()).id());
    }
    ic_data.AddCheck(class_ids, target_function);
  }
  if (FLAG_trace_ic_miss_in_optimized) {
    const Code& caller = Code::Handle(Code::LookupCode(caller_frame->pc()));
    if (caller.is_optimized()) {
      OS::Print("IC miss in optimized code; call %s -> %s\n",
          Function::Handle(caller.function()).ToCString(),
          target_function.ToCString());
    }
  }
  if (FLAG_trace_ic) {
    OS::Print("InlineCacheMissHandler %d call at %#"Px"' "
              "adding <%s> id:%"Pd" -> <%s>\n",
        args.length(),
        caller_frame->pc(),
        Class::Handle(receiver.clazz()).ToCString(),
        Class::Handle(receiver.clazz()).id(),
        target_function.ToCString());
  }
  return target_function.raw();
}


// Handles inline cache misses by updating the IC data array of the call
// site.
//   Arg0: Receiver object.
//   Returns: target function with compiled code or null.
// Modifies the instance call to hold the updated IC data array.
DEFINE_RUNTIME_ENTRY(InlineCacheMissHandlerOneArg, 1) {
  ASSERT(arguments.ArgCount() ==
      kInlineCacheMissHandlerOneArgRuntimeEntry.argument_count());
  const Instance& receiver = Instance::CheckedHandle(arguments.ArgAt(0));
  GrowableArray<const Instance*> args(1);
  args.Add(&receiver);
  const Function& result =
      Function::Handle(InlineCacheMissHandler(isolate, args));
  arguments.SetReturn(result);
}


// Handles inline cache misses by updating the IC data array of the call
// site.
//   Arg0: Receiver object.
//   Arg1: Argument after receiver.
//   Returns: target function with compiled code or null.
// Modifies the instance call to hold the updated IC data array.
DEFINE_RUNTIME_ENTRY(InlineCacheMissHandlerTwoArgs, 2) {
  ASSERT(arguments.ArgCount() ==
      kInlineCacheMissHandlerTwoArgsRuntimeEntry.argument_count());
  const Instance& receiver = Instance::CheckedHandle(arguments.ArgAt(0));
  const Instance& other = Instance::CheckedHandle(arguments.ArgAt(1));
  GrowableArray<const Instance*> args(2);
  args.Add(&receiver);
  args.Add(&other);
  const Function& result =
      Function::Handle(InlineCacheMissHandler(isolate, args));
  arguments.SetReturn(result);
}


// Handles inline cache misses by updating the IC data array of the call
// site.
//   Arg0: Receiver object.
//   Arg1: Argument after receiver.
//   Arg2: Second argument after receiver.
//   Returns: target function with compiled code or null.
// Modifies the instance call to hold the updated IC data array.
DEFINE_RUNTIME_ENTRY(InlineCacheMissHandlerThreeArgs, 3) {
  ASSERT(arguments.ArgCount() ==
      kInlineCacheMissHandlerThreeArgsRuntimeEntry.argument_count());
  const Instance& receiver = Instance::CheckedHandle(arguments.ArgAt(0));
  const Instance& arg1 = Instance::CheckedHandle(arguments.ArgAt(1));
  const Instance& arg2 = Instance::CheckedHandle(arguments.ArgAt(2));
  GrowableArray<const Instance*> args(3);
  args.Add(&receiver);
  args.Add(&arg1);
  args.Add(&arg2);
  const Function& result =
      Function::Handle(InlineCacheMissHandler(isolate, args));
  arguments.SetReturn(result);
}


// Updates IC data for two arguments. Used by the equality operation when
// the control flow bypasses regular inline cache (null arguments).
//   Arg0: Receiver object.
//   Arg1: Argument after receiver.
//   Arg2: Target's name.
//   Arg3: ICData.
DEFINE_RUNTIME_ENTRY(UpdateICDataTwoArgs, 4) {
  ASSERT(arguments.ArgCount() ==
      kUpdateICDataTwoArgsRuntimeEntry.argument_count());
  const Instance& receiver = Instance::CheckedHandle(arguments.ArgAt(0));
  const Instance& arg1 = Instance::CheckedHandle(arguments.ArgAt(1));
  const String& target_name = String::CheckedHandle(arguments.ArgAt(2));
  const ICData& ic_data = ICData::CheckedHandle(arguments.ArgAt(3));
  GrowableArray<const Instance*> args(2);
  args.Add(&receiver);
  args.Add(&arg1);
  const intptr_t kNumArguments = 2;
  const intptr_t kNumNamedArguments = 0;
  Function& target_function = Function::Handle();
  target_function = Resolver::ResolveDynamic(receiver,
                                             target_name,
                                             kNumArguments,
                                             kNumNamedArguments);
  ASSERT(!target_function.IsNull());
  GrowableArray<intptr_t> class_ids(kNumArguments);
  ASSERT(ic_data.num_args_tested() == kNumArguments);
  class_ids.Add(Class::Handle(receiver.clazz()).id());
  class_ids.Add(Class::Handle(arg1.clazz()).id());
  ic_data.AddCheck(class_ids, target_function);
}


static RawFunction* LookupDynamicFunction(Isolate* isolate,
                                          const Class& in_cls,
                                          const String& name) {
  Class& cls = Class::Handle();
  // For lookups treat null as an instance of class Object.
  if (in_cls.IsNullClass()) {
    cls = isolate->object_store()->object_class();
  } else {
    cls = in_cls.raw();
  }

  Function& function = Function::Handle();
  while (!cls.IsNull()) {
    // Check if function exists.
    function = cls.LookupDynamicFunction(name);
    if (!function.IsNull()) {
      break;
    }
    cls = cls.SuperClass();
  }
  return function.raw();
}


// Resolve an implicit closure by checking if an instance function
// of the same name exists and creating a closure object of the function.
// Arg0: receiver object.
// Arg1: ic-data.
// Returns: Closure object or NULL (instance function not found).
// This is called by the megamorphic stub when it is unable to resolve an
// instance method. This is done just before the call to noSuchMethod.
DEFINE_RUNTIME_ENTRY(ResolveImplicitClosureFunction, 2) {
  ASSERT(arguments.ArgCount() ==
         kResolveImplicitClosureFunctionRuntimeEntry.argument_count());
  const Instance& receiver = Instance::CheckedHandle(arguments.ArgAt(0));
  const ICData& ic_data = ICData::CheckedHandle(arguments.ArgAt(1));
  const String& original_function_name = String::Handle(ic_data.target_name());
  Instance& closure = Instance::Handle();
  if (!Field::IsGetterName(original_function_name)) {
    // This is not a getter so can't be the case where we are trying to
    // create an implicit closure of an instance function.
    arguments.SetReturn(closure);
    return;
  }
  const Class& receiver_class = Class::Handle(receiver.clazz());
  ASSERT(!receiver_class.IsNull());
  String& func_name = String::Handle();
  func_name = Field::NameFromGetter(original_function_name);
  func_name = Symbols::New(func_name);
  const Function& function = Function::Handle(
      LookupDynamicFunction(isolate, receiver_class, func_name));
  if (function.IsNull()) {
    // There is no function of the same name so can't be the case where
    // we are trying to create an implicit closure of an instance function.
    arguments.SetReturn(closure);
    return;
  }
  Function& implicit_closure_function =
      Function::Handle(function.ImplicitClosureFunction());
  // Create a closure object for the implicit closure function.
  const Context& context = Context::Handle(Context::New(1));
  context.SetAt(0, receiver);
  closure = Closure::New(implicit_closure_function, context);
  if (receiver_class.HasTypeArguments()) {
    const AbstractTypeArguments& type_arguments =
        AbstractTypeArguments::Handle(receiver.GetTypeArguments());
    closure.SetTypeArguments(type_arguments);
  }
  arguments.SetReturn(closure);
}


// Resolve an implicit closure by invoking getter and checking if the return
// value from getter is a closure.
// Arg0: receiver object.
// Arg1: ic-data.
// Returns: Closure object or NULL (closure not found).
// This is called by the megamorphic stub when it is unable to resolve an
// instance method. This is done just before the call to noSuchMethod.
DEFINE_RUNTIME_ENTRY(ResolveImplicitClosureThroughGetter, 2) {
  ASSERT(arguments.ArgCount() ==
         kResolveImplicitClosureThroughGetterRuntimeEntry.argument_count());
  const Instance& receiver = Instance::CheckedHandle(arguments.ArgAt(0));
  const ICData& ic_data = ICData::CheckedHandle(arguments.ArgAt(1));
  const String& original_function_name = String::Handle(ic_data.target_name());
  const int kNumArguments = 1;
  const int kNumNamedArguments = 0;
  const String& getter_function_name =
      String::Handle(Field::GetterName(original_function_name));
  Function& function = Function::ZoneHandle(
      Resolver::ResolveDynamic(receiver,
                               getter_function_name,
                               kNumArguments,
                               kNumNamedArguments));
  Code& code = Code::Handle();
  if (function.IsNull()) {
    arguments.SetReturn(code);
    return;  // No getter function found so can't be an implicit closure.
  }
  GrowableArray<const Object*> invoke_arguments(0);
  const Array& kNoArgumentNames = Array::Handle();
  const Object& result =
      Object::Handle(DartEntry::InvokeDynamic(receiver,
                                              function,
                                              invoke_arguments,
                                              kNoArgumentNames));
  if (result.IsError()) {
    if (result.IsUnhandledException()) {
      // If the getter throws an exception, treat as no such method.
      arguments.SetReturn(code);
      return;
    } else {
      Exceptions::PropagateError(Error::Cast(result));
    }
  }
  if (!result.IsSmi()) {
    const Class& cls = Class::Handle(result.clazz());
    ASSERT(!cls.IsNull());
    function = cls.signature_function();
    if (!function.IsNull()) {
      arguments.SetReturn(result);
      return;  // Return closure object.
    }
  }
  // The result instance is not a closure, try to invoke method "call" before
  // throwing a NoSuchMethodError.

  // TODO(regis): Factorize the following code.

  // TODO(regis): Args should be passed.
  const Array& function_args = Array::Handle();
  const String& function_name = String::Handle(Symbols::Call());
  GrowableArray<const Object*> dart_arguments(5);

  // TODO(regis): Resolve and invoke "call" method, if existing.

  const Object& null_object = Object::Handle();
  dart_arguments.Add(&result);
  dart_arguments.Add(&function_name);
  dart_arguments.Add(&function_args);
  dart_arguments.Add(&null_object);

  // Report if a function "call" with different arguments has been found.
  {
    Class& instance_class = Class::Handle(result.clazz());
    Function& function =
        Function::Handle(instance_class.LookupDynamicFunction(function_name));
    while (function.IsNull()) {
      instance_class = instance_class.SuperClass();
      if (instance_class.IsNull()) break;
      function = instance_class.LookupDynamicFunction(function_name);
    }
    if (!function.IsNull()) {
      const int total_num_parameters = function.NumParameters();
      const Array& array = Array::Handle(Array::New(total_num_parameters - 1));
      // Skip receiver.
      for (int i = 1; i < total_num_parameters; i++) {
        array.SetAt(i - 1, String::Handle(function.ParameterNameAt(i)));
      }
      dart_arguments.Add(&array);
    }
  }
  Exceptions::ThrowByType(Exceptions::kNoSuchMethod, dart_arguments);
  UNREACHABLE();
}


// Invoke Implicit Closure function.
// Arg0: closure object.
// Arg1: arguments descriptor (originally passed as dart instance invocation).
// Arg2: arguments array (originally passed to dart instance invocation).
DEFINE_RUNTIME_ENTRY(InvokeImplicitClosureFunction, 3) {
  ASSERT(arguments.ArgCount() ==
         kInvokeImplicitClosureFunctionRuntimeEntry.argument_count());
  const Instance& closure = Instance::CheckedHandle(arguments.ArgAt(0));
  const Array& arg_descriptor = Array::CheckedHandle(arguments.ArgAt(1));
  const Array& func_arguments = Array::CheckedHandle(arguments.ArgAt(2));
  const Function& function = Function::Handle(Closure::function(closure));
  ASSERT(!function.IsNull());
  if (!function.HasCode()) {
    const Error& error = Error::Handle(Compiler::CompileFunction(function));
    if (!error.IsNull()) {
      Exceptions::PropagateError(error);
    }
  }
  const Context& context = Context::Handle(Closure::context(closure));
  const Code& code = Code::Handle(function.CurrentCode());
  ASSERT(!code.IsNull());
  const Instructions& instrs = Instructions::Handle(code.instructions());
  ASSERT(!instrs.IsNull());

  // Receiver parameter has already been skipped by caller.
  // The closure object is passed as implicit first argument to closure
  // functions, since it may be needed to throw a NoSuchMethodError, in case
  // the wrong number of arguments is passed.
  GrowableArray<const Object*> invoke_arguments(func_arguments.Length() + 1);
  invoke_arguments.Add(&closure);
  for (intptr_t i = 0; i < func_arguments.Length(); i++) {
    const Object& value = Object::Handle(func_arguments.At(i));
    invoke_arguments.Add(&value);
  }

  // Now Call the invoke stub which will invoke the closure.
  DartEntry::invokestub entrypoint = reinterpret_cast<DartEntry::invokestub>(
      StubCode::InvokeDartCodeEntryPoint());
  ASSERT(context.isolate() == Isolate::Current());
  const Object& result = Object::Handle(
      entrypoint(instrs.EntryPoint(),
                 arg_descriptor,
                 invoke_arguments.data(),
                 context));
  CheckResultError(result);
  arguments.SetReturn(result);
}


// Invoke appropriate noSuchMethod function.
// Arg0: receiver.
// Arg1: ic-data.
// Arg2: original arguments descriptor array.
// Arg3: original arguments array.
DEFINE_RUNTIME_ENTRY(InvokeNoSuchMethodFunction, 4) {
  ASSERT(arguments.ArgCount() ==
         kInvokeNoSuchMethodFunctionRuntimeEntry.argument_count());
  const Instance& receiver = Instance::CheckedHandle(arguments.ArgAt(0));
  const ICData& ic_data = ICData::CheckedHandle(arguments.ArgAt(1));
  const String& original_function_name = String::Handle(ic_data.target_name());
  ASSERT(!Array::CheckedHandle(arguments.ArgAt(2)).IsNull());
  const Array& orig_arguments = Array::CheckedHandle(arguments.ArgAt(3));
  // Allocate an InvocationMirror object.
  // TODO(regis): Fill in the InvocationMirror object correctly at
  // this point we do not deal with named arguments and treat them
  // all as positional.
  const Library& core_lib = Library::Handle(Library::CoreLibrary());
  const String& invocation_mirror_name = String::Handle(
      Symbols::InvocationMirror());
  Class& invocation_mirror_class = Class::Handle(
      core_lib.LookupClassAllowPrivate(invocation_mirror_name));
  ASSERT(!invocation_mirror_class.IsNull());
  const String& allocation_function_name = String::Handle(
      Symbols::AllocateInvocationMirror());
  const Function& allocation_function = Function::ZoneHandle(
      Resolver::ResolveStaticByName(invocation_mirror_class,
                                    allocation_function_name,
                                    Resolver::kIsQualified));
  ASSERT(!allocation_function.IsNull());
  GrowableArray<const Object*> allocation_arguments(2);
  allocation_arguments.Add(&original_function_name);
  allocation_arguments.Add(&orig_arguments);
  const Array& kNoArgumentNames = Array::Handle();
  const Object& invocation_mirror = Object::Handle(
      DartEntry::InvokeStatic(allocation_function,
                              allocation_arguments,
                              kNoArgumentNames));

  const int kNumArguments = 2;
  const int kNumNamedArguments = 0;
  const String& function_name = String::Handle(Symbols::NoSuchMethod());
  const Function& function = Function::ZoneHandle(
      Resolver::ResolveDynamic(receiver,
                               function_name,
                               kNumArguments,
                               kNumNamedArguments));
  ASSERT(!function.IsNull());
  GrowableArray<const Object*> invoke_arguments(1);
  invoke_arguments.Add(&invocation_mirror);
  const Object& result = Object::Handle(
      DartEntry::InvokeDynamic(receiver,
                               function,
                               invoke_arguments,
                               kNoArgumentNames));
  CheckResultError(result);
  arguments.SetReturn(result);
}


// A non-closure object was invoked as a closure, so call the "call" method
// on it.
// Arg0: non-closure object.
// Arg1: arguments array.
// TODO(regis): Rename this entry?
DEFINE_RUNTIME_ENTRY(ReportObjectNotClosure, 2) {
  ASSERT(arguments.ArgCount() ==
         kReportObjectNotClosureRuntimeEntry.argument_count());
  const Instance& instance = Instance::CheckedHandle(arguments.ArgAt(0));
  const Array& function_args = Array::CheckedHandle(arguments.ArgAt(1));
  const String& function_name = String::Handle(Symbols::Call());
  GrowableArray<const Object*> dart_arguments(5);

  // TODO(regis): Resolve and invoke "call" method, if existing.

  const Object& null_object = Object::Handle();
  dart_arguments.Add(&instance);
  dart_arguments.Add(&function_name);
  dart_arguments.Add(&function_args);
  dart_arguments.Add(&null_object);

  // Report if a function "call" with different arguments has been found.
  Class& instance_class = Class::Handle(instance.clazz());
  Function& function =
      Function::Handle(instance_class.LookupDynamicFunction(function_name));
  while (function.IsNull()) {
    instance_class = instance_class.SuperClass();
    if (instance_class.IsNull()) break;
    function = instance_class.LookupDynamicFunction(function_name);
  }
  if (!function.IsNull()) {
    const int total_num_parameters = function.NumParameters();
    const Array& array = Array::Handle(Array::New(total_num_parameters - 1));
    // Skip receiver.
    for (int i = 1; i < total_num_parameters; i++) {
      array.SetAt(i - 1, String::Handle(function.ParameterNameAt(i)));
    }
    dart_arguments.Add(&array);
  }
  Exceptions::ThrowByType(Exceptions::kNoSuchMethod, dart_arguments);
  UNREACHABLE();
}


// A closure object was invoked with incompatible arguments.
// TODO(regis): Deprecated. This case should be handled by a noSuchMethod call.
DEFINE_RUNTIME_ENTRY(ClosureArgumentMismatch, 0) {
  ASSERT(arguments.ArgCount() ==
         kClosureArgumentMismatchRuntimeEntry.argument_count());
  const Instance& instance = Instance::Handle();  // Incorrect. OK for now.
  const Array& function_args = Array::Handle();  // Incorrect. OK for now.
  const String& function_name = String::Handle(Symbols::Call());
  GrowableArray<const Object*> dart_arguments(5);

  const Object& null_object = Object::Handle();
  dart_arguments.Add(&instance);
  dart_arguments.Add(&function_name);
  dart_arguments.Add(&function_args);
  dart_arguments.Add(&null_object);
  Exceptions::ThrowByType(Exceptions::kNoSuchMethod, dart_arguments);
  UNREACHABLE();
}


DEFINE_RUNTIME_ENTRY(StackOverflow, 0) {
  ASSERT(arguments.ArgCount() ==
         kStackOverflowRuntimeEntry.argument_count());
  uword stack_pos = reinterpret_cast<uword>(&arguments);

  // If an interrupt happens at the same time as a stack overflow, we
  // process the stack overflow first.
  if (stack_pos < isolate->saved_stack_limit()) {
    // Use the preallocated stack overflow exception to avoid calling
    // into dart code.
    const Instance& exception =
        Instance::Handle(isolate->object_store()->stack_overflow());
    Exceptions::Throw(exception);
    UNREACHABLE();
  }

  uword interrupt_bits = isolate->GetAndClearInterrupts();
  if (interrupt_bits & Isolate::kStoreBufferInterrupt) {
    if (FLAG_verbose_gc) {
      OS::PrintErr("Scavenge scheduled by store buffer overflow.\n");
    }
    isolate->heap()->CollectGarbage(Heap::kNew);
  }
  if (interrupt_bits & Isolate::kMessageInterrupt) {
    isolate->message_handler()->HandleOOBMessages();
  }
  if (interrupt_bits & Isolate::kApiInterrupt) {
    // Signal isolate interrupt  event.
    Debugger::SignalIsolateEvent(Debugger::kIsolateInterrupted);

    Dart_IsolateInterruptCallback callback = isolate->InterruptCallback();
    if (callback) {
      if ((*callback)()) {
        return;
      } else {
        // TODO(turnidge): Unwind the stack.
        UNIMPLEMENTED();
      }
    }
  }
}


static void PrintCaller(const char* msg) {
  DartFrameIterator iterator;
  StackFrame* top_frame = iterator.NextFrame();
  ASSERT(top_frame != NULL);
  const Function& top_function = Function::Handle(
      top_frame->LookupDartFunction());
  OS::Print("Failed: '%s' %s @ %#"Px"\n",
      msg, top_function.ToFullyQualifiedCString(), top_frame->pc());
  StackFrame* caller_frame = iterator.NextFrame();
  if (caller_frame != NULL) {
    const Function& caller_function = Function::Handle(
        caller_frame->LookupDartFunction());
    const Code& code = Code::Handle(caller_frame->LookupDartCode());
    OS::Print("  -> caller: %s (%s)\n",
        caller_function.ToFullyQualifiedCString(),
        code.is_optimized() ? "optimized" : "unoptimized");
  }
}


DEFINE_RUNTIME_ENTRY(TraceICCall, 2) {
  ASSERT(arguments.ArgCount() ==
         kTraceICCallRuntimeEntry.argument_count());
  const ICData& ic_data = ICData::CheckedHandle(arguments.ArgAt(0));
  const Function& function = Function::CheckedHandle(arguments.ArgAt(1));
  DartFrameIterator iterator;
  StackFrame* frame = iterator.NextFrame();
  ASSERT(frame != NULL);
  OS::Print("IC call @%#"Px": ICData: %p cnt:%"Pd" nchecks: %"Pd" %s %s\n",
      frame->pc(),
      ic_data.raw(),
      function.usage_counter(),
      ic_data.NumberOfChecks(),
      ic_data.is_closure_call() ? "closure" : "",
      function.ToFullyQualifiedCString());
}


// This is called from function that needs to be optimized.
// The requesting function can be already optimized (reoptimization).
DEFINE_RUNTIME_ENTRY(OptimizeInvokedFunction, 1) {
  ASSERT(arguments.ArgCount() ==
         kOptimizeInvokedFunctionRuntimeEntry.argument_count());
  const intptr_t kLowInvocationCount = -100000000;
  const Function& function = Function::CheckedHandle(arguments.ArgAt(0));
  if (isolate->debugger()->IsActive()) {
    // We cannot set breakpoints in optimized code, so do not optimize
    // the function.
    function.set_usage_counter(0);
    return;
  }
  if (function.deoptimization_counter() >=
      FLAG_deoptimization_counter_threshold) {
    if (FLAG_trace_failed_optimization_attempts) {
      PrintCaller("Too Many Deoptimizations");
    }
    // TODO(srdjan): Investigate excessive deoptimization.
    function.set_usage_counter(kLowInvocationCount);
    return;
  }
  if ((FLAG_optimization_filter != NULL) &&
      (strstr(function.ToFullyQualifiedCString(),
              FLAG_optimization_filter) == NULL)) {
    function.set_usage_counter(kLowInvocationCount);
    return;
  }
  if (function.is_optimizable()) {
    const Error& error =
        Error::Handle(Compiler::CompileOptimizedFunction(function));
    if (!error.IsNull()) {
      Exceptions::PropagateError(error);
    }
    const Code& optimized_code = Code::Handle(function.CurrentCode());
    ASSERT(!optimized_code.IsNull());
    // Set usage counter for reoptimization.
    function.set_usage_counter(
        function.usage_counter() - FLAG_reoptimization_counter_threshold);
  } else {
    if (FLAG_trace_failed_optimization_attempts) {
      PrintCaller("Not Optimizable");
    }
    // TODO(5442338): Abort as this should not happen.
    function.set_usage_counter(kLowInvocationCount);
  }
}


// The caller must be a static call in a Dart frame, or an entry frame.
// Patch static call to point to valid code's entry point.
DEFINE_RUNTIME_ENTRY(FixCallersTarget, 0) {
  ASSERT(arguments.ArgCount() ==
      kFixCallersTargetRuntimeEntry.argument_count());

  StackFrameIterator iterator(StackFrameIterator::kDontValidateFrames);
  StackFrame* frame = iterator.NextFrame();
  while (frame != NULL && (frame->IsStubFrame() || frame->IsExitFrame())) {
    frame = iterator.NextFrame();
  }
  ASSERT(frame != NULL);
  if (frame->IsEntryFrame()) {
    // Since function's current code is always unpatched, the entry frame always
    // calls to unpatched code.
    UNREACHABLE();
  }
  ASSERT(frame->IsDartFrame());
  const Code& caller_code = Code::Handle(frame->LookupDartCode());
  const Function& target_function = Function::Handle(
      caller_code.GetStaticCallTargetFunctionAt(frame->pc()));
  const Code& target_code = Code::Handle(target_function.CurrentCode());
  CodePatcher::PatchStaticCallAt(frame->pc(), target_code.EntryPoint());
  caller_code.SetStaticCallTargetCodeAt(frame->pc(), target_code);
  if (FLAG_trace_patching) {
    OS::Print("FixCallersTarget: patching from %#"Px" to '%s' %#"Px"\n",
        frame->pc(),
        Function::Handle(target_code.function()).ToFullyQualifiedCString(),
        target_code.EntryPoint());
  }
  arguments.SetReturn(target_code);
}


const char* DeoptReasonToText(intptr_t deopt_id) {
  switch (deopt_id) {
#define DEOPT_REASON_ID_TO_TEXT(name) case kDeopt##name: return #name;
DEOPT_REASONS(DEOPT_REASON_ID_TO_TEXT)
#undef DEOPT_REASON_ID_TO_TEXT
    default:
      UNREACHABLE();
      return "";
  }
}


static void GetDeoptInfoAtPc(const Code& code,
                             uword pc,
                             DeoptInfo* deopt_info,
                             DeoptReasonId* deopt_reason) {
  ASSERT(code.is_optimized());
  const Instructions& instructions = Instructions::Handle(code.instructions());
  uword code_entry = instructions.EntryPoint();
  const Array& table = Array::Handle(code.deopt_info_array());
  ASSERT(!table.IsNull());
  // Linear search for the PC offset matching the target PC.
  intptr_t length = DeoptTable::GetLength(table);
  Smi& offset = Smi::Handle();
  Smi& reason = Smi::Handle();
  for (intptr_t i = 0; i < length; ++i) {
    DeoptTable::GetEntry(table, i, &offset, deopt_info, &reason);
    if (pc == (code_entry + offset.Value())) {
      *deopt_reason = static_cast<DeoptReasonId>(reason.Value());
      return;
    }
  }
  *deopt_info = DeoptInfo::null();
  *deopt_reason = kDeoptUnknown;
}


static void DeoptimizeAt(const Code& optimized_code, uword pc) {
  DeoptInfo& deopt_info = DeoptInfo::Handle();
  DeoptReasonId deopt_reason = kDeoptUnknown;
  GetDeoptInfoAtPc(optimized_code, pc, &deopt_info, &deopt_reason);
  ASSERT(!deopt_info.IsNull());
  const Function& function = Function::Handle(optimized_code.function());
  const Code& unoptimized_code = Code::Handle(function.unoptimized_code());
  ASSERT(!unoptimized_code.IsNull());
  // The switch to unoptimized code may have already occured.
  if (function.HasOptimizedCode()) {
    function.SwitchToUnoptimizedCode();
  }
  // Patch call site (lazy deoptimization is quite rare, patching it twice
  // is not a performance issue).
  uword lazy_deopt_jump = optimized_code.GetLazyDeoptPc();
  ASSERT(lazy_deopt_jump != 0);
  CodePatcher::InsertCallAt(pc, lazy_deopt_jump);
  // Mark code as dead (do not GC its embedded objects).
  optimized_code.set_is_alive(false);
}


// Currently checks only that all optimized frames have kDeoptIndex
// and unoptimized code has the kDeoptAfter.
void DeoptimizeAll() {
  DartFrameIterator iterator;
  StackFrame* frame = iterator.NextFrame();
  Code& optimized_code = Code::Handle();
  while (frame != NULL) {
    optimized_code = frame->LookupDartCode();
    if (optimized_code.is_optimized()) {
      DeoptimizeAt(optimized_code, frame->pc());
    }
    frame = iterator.NextFrame();
  }
}


// Returns true if the given array of cids contains the given cid.
static bool ContainsCid(const GrowableArray<intptr_t>& cids, intptr_t cid) {
  for (intptr_t i = 0; i < cids.length(); i++) {
    if (cids[i] == cid) {
      return true;
    }
  }
  return false;
}


// Deoptimize optimized code on stack if its class is in the 'classes' array.
void DeoptimizeIfOwner(const GrowableArray<intptr_t>& classes) {
  DartFrameIterator iterator;
  StackFrame* frame = iterator.NextFrame();
  Code& optimized_code = Code::Handle();
  while (frame != NULL) {
    optimized_code = frame->LookupDartCode();
    if (optimized_code.is_optimized()) {
      const intptr_t owner_cid = Class::Handle(Function::Handle(
          optimized_code.function()).Owner()).id();
      if (ContainsCid(classes, owner_cid)) {
        DeoptimizeAt(optimized_code, frame->pc());
      }
    }
  }
}


// Copy saved registers into the isolate buffer.
static void CopySavedRegisters(uword saved_registers_address) {
  double* xmm_registers_copy = new double[kNumberOfXmmRegisters];
  ASSERT(xmm_registers_copy != NULL);
  for (intptr_t i = 0; i < kNumberOfXmmRegisters; i++) {
    xmm_registers_copy[i] = *reinterpret_cast<double*>(saved_registers_address);
    saved_registers_address += kDoubleSize;
  }
  Isolate::Current()->set_deopt_xmm_registers_copy(xmm_registers_copy);

  intptr_t* cpu_registers_copy = new intptr_t[kNumberOfCpuRegisters];
  ASSERT(cpu_registers_copy != NULL);
  for (intptr_t i = 0; i < kNumberOfCpuRegisters; i++) {
    cpu_registers_copy[i] =
        *reinterpret_cast<intptr_t*>(saved_registers_address);
    saved_registers_address += kWordSize;
  }
  Isolate::Current()->set_deopt_cpu_registers_copy(cpu_registers_copy);
}


// Copy optimized frame into the isolate buffer.
// The first incoming argument is stored at the last entry in the
// copied frame buffer.
static void CopyFrame(const Code& optimized_code, const StackFrame& frame) {
  const Function& function = Function::Handle(optimized_code.function());
  // Do not copy incoming arguments if there are optional arguments (they
  // are copied into local space at method entry).
  const intptr_t num_args =
      function.HasOptionalParameters() ? 0 : function.num_fixed_parameters();
  // FP, PC-marker and return-address will be copied as well.
  const intptr_t frame_copy_size =
      1  // Deoptimized function's return address: caller_frame->pc().
      + ((frame.fp() - frame.sp()) / kWordSize)
      + 1  // PC marker.
      + 1  // Caller return address.
      + num_args;
  intptr_t* frame_copy = new intptr_t[frame_copy_size];
  ASSERT(frame_copy != NULL);
  // Include the return address of optimized code.
  intptr_t* start = reinterpret_cast<intptr_t*>(frame.sp() - kWordSize);
  for (intptr_t i = 0; i < frame_copy_size; i++) {
    frame_copy[i] = *(start + i);
  }
  Isolate::Current()->SetDeoptFrameCopy(frame_copy, frame_copy_size);
}


// Copies saved registers and caller's frame into temporary buffers.
// Returns the stack size of unoptimized frame.
DEFINE_LEAF_RUNTIME_ENTRY(intptr_t, DeoptimizeCopyFrame,
                          uword saved_registers_address) {
  Isolate* isolate = Isolate::Current();
  StackZone zone(isolate);
  HANDLESCOPE(isolate);

  // All registers have been saved below last-fp.
  const uword last_fp = saved_registers_address +
      kNumberOfCpuRegisters * kWordSize + kNumberOfXmmRegisters * kDoubleSize;
  CopySavedRegisters(saved_registers_address);

  // Get optimized code and frame that need to be deoptimized.
  DartFrameIterator iterator(last_fp);
  StackFrame* caller_frame = iterator.NextFrame();
  ASSERT(caller_frame != NULL);
  const Code& optimized_code = Code::Handle(caller_frame->LookupDartCode());
  ASSERT(optimized_code.is_optimized());


  DeoptInfo& deopt_info = DeoptInfo::Handle();
  DeoptReasonId deopt_reason = kDeoptUnknown;
  GetDeoptInfoAtPc(optimized_code, caller_frame->pc(), &deopt_info,
                   &deopt_reason);
  ASSERT(!deopt_info.IsNull());

  CopyFrame(optimized_code, *caller_frame);
  if (FLAG_trace_deoptimization) {
    Function& function = Function::Handle(optimized_code.function());
    OS::Print("Deoptimizing (reason %d '%s') at pc %#"Px" '%s' (count %d)\n",
        deopt_reason,
        DeoptReasonToText(deopt_reason),
        caller_frame->pc(),
        function.ToFullyQualifiedCString(),
        function.deoptimization_counter());
  }

  // Compute the stack size of the unoptimized frame.  For functions with
  // optional arguments the deoptimization info does not describe the
  // incoming arguments.
  const Function& function = Function::Handle(optimized_code.function());
  const intptr_t num_args =
      function.HasOptionalParameters() ? 0 : function.num_fixed_parameters();
  intptr_t unoptimized_stack_size =
      + deopt_info.TranslationLength() - num_args
      - 2;  // Subtract caller FP and PC.
  return unoptimized_stack_size * kWordSize;
}
END_LEAF_RUNTIME_ENTRY



static intptr_t DeoptimizeWithDeoptInfo(const Code& code,
                                        const DeoptInfo& deopt_info,
                                        const StackFrame& caller_frame,
                                        DeoptReasonId deopt_reason) {
  const intptr_t len = deopt_info.TranslationLength();
  GrowableArray<DeoptInstr*> deopt_instructions(len);
  const Array& deopt_table = Array::Handle(code.deopt_info_array());
  ASSERT(!deopt_table.IsNull());
  deopt_info.ToInstructions(deopt_table, &deopt_instructions);

  intptr_t* start = reinterpret_cast<intptr_t*>(caller_frame.sp() - kWordSize);
  const Function& function = Function::Handle(code.function());
  const intptr_t num_args =
      function.HasOptionalParameters() ? 0 : function.num_fixed_parameters();
  intptr_t to_frame_size =
      1  // Deoptimized function's return address.
      + (caller_frame.fp() - caller_frame.sp()) / kWordSize
      + 3  // caller-fp, pc, pc-marker.
      + num_args;
  DeoptimizationContext deopt_context(start,
                                      to_frame_size,
                                      Array::Handle(code.object_table()),
                                      num_args,
                                      deopt_reason);
  for (intptr_t to_index = len - 1; to_index >= 0; to_index--) {
    deopt_instructions[to_index]->Execute(&deopt_context, to_index);
  }
  if (FLAG_trace_deoptimization_verbose) {
    for (intptr_t i = 0; i < len; i++) {
      OS::Print("*%"Pd". [%p] %#014"Px" [%s]\n",
          i,
          &start[i],
          start[i],
          deopt_instructions[i]->ToCString());
    }
  }
  return deopt_context.GetCallerFp();
}


// The stack has been adjusted to fit all values for unoptimized frame.
// Fill the unoptimized frame.
DEFINE_LEAF_RUNTIME_ENTRY(intptr_t, DeoptimizeFillFrame, uword last_fp) {
  Isolate* isolate = Isolate::Current();
  StackZone zone(isolate);
  HANDLESCOPE(isolate);

  DartFrameIterator iterator(last_fp);
  StackFrame* caller_frame = iterator.NextFrame();
  ASSERT(caller_frame != NULL);
  const Code& optimized_code = Code::Handle(caller_frame->LookupDartCode());
  const Function& function = Function::Handle(optimized_code.function());
  ASSERT(!function.IsNull());
  const Code& unoptimized_code = Code::Handle(function.unoptimized_code());
  ASSERT(!optimized_code.IsNull() && optimized_code.is_optimized());
  ASSERT(!unoptimized_code.IsNull() && !unoptimized_code.is_optimized());

  intptr_t* frame_copy = isolate->deopt_frame_copy();
  intptr_t* cpu_registers_copy = isolate->deopt_cpu_registers_copy();
  double* xmm_registers_copy = isolate->deopt_xmm_registers_copy();

  DeoptInfo& deopt_info = DeoptInfo::Handle();
  DeoptReasonId deopt_reason = kDeoptUnknown;
  GetDeoptInfoAtPc(optimized_code, caller_frame->pc(), &deopt_info,
                   &deopt_reason);
  ASSERT(!deopt_info.IsNull());

  const intptr_t caller_fp =
      DeoptimizeWithDeoptInfo(optimized_code, deopt_info, *caller_frame,
                              deopt_reason);

  isolate->SetDeoptFrameCopy(NULL, 0);
  isolate->set_deopt_cpu_registers_copy(NULL);
  isolate->set_deopt_xmm_registers_copy(NULL);
  delete[] frame_copy;
  delete[] cpu_registers_copy;
  delete[] xmm_registers_copy;

  return caller_fp;
}
END_LEAF_RUNTIME_ENTRY


// This is the last step in the deoptimization, GC can occur.
DEFINE_RUNTIME_ENTRY(DeoptimizeMaterializeDoubles, 0) {
  DeferredDouble* deferred_double = Isolate::Current()->DetachDeferredDoubles();

  while (deferred_double != NULL) {
    DeferredDouble* current = deferred_double;
    deferred_double = deferred_double->next();

    RawDouble** slot = current->slot();
    *slot = Double::New(current->value());

    if (FLAG_trace_deoptimization_verbose) {
      OS::Print("materializing double at %"Px": %g\n",
                reinterpret_cast<uword>(current->slot()),
                current->value());
    }

    delete current;
  }

  DeferredMint* deferred_mint = Isolate::Current()->DetachDeferredMints();

  while (deferred_mint != NULL) {
    DeferredMint* current = deferred_mint;
    deferred_mint = deferred_mint->next();

    RawMint** slot = current->slot();
    ASSERT(!Smi::IsValid64(current->value()));
    *slot = Mint::New(current->value());

    if (FLAG_trace_deoptimization_verbose) {
      OS::Print("materializing mint at %"Px": %"Pd64"\n",
                reinterpret_cast<uword>(current->slot()),
                current->value());
    }

    delete current;
  }
  // Since this is the only step where GC can occur during deoptimization,
  // use it to report the source line where deoptimization occured.
  if (FLAG_trace_deoptimization) {
    DartFrameIterator iterator;
    StackFrame* top_frame = iterator.NextFrame();
    ASSERT(top_frame != NULL);
    const Code& code = Code::Handle(top_frame->LookupDartCode());
    const Function& top_function = Function::Handle(code.function());
    const Script& script = Script::Handle(top_function.script());
    const intptr_t token_pos = code.GetTokenIndexOfPC(top_frame->pc());
    intptr_t line, column;
    script.GetTokenLocation(token_pos, &line, &column);
    String& line_string = String::Handle(script.GetLine(line));
    OS::Print("  Function: %s\n", top_function.ToFullyQualifiedCString());
    OS::Print("  Line %"Pd": '%s'\n", line, line_string.ToCString());
  }
}


}  // namespace dart
