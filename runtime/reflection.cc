/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "reflection.h"

#include "class_linker.h"
#include "common_throws.h"
#include "dex_file-inl.h"
#include "jni_internal.h"
#include "mirror/art_field-inl.h"
#include "mirror/art_method-inl.h"
#include "mirror/class.h"
#include "mirror/class-inl.h"
#include "mirror/object_array.h"
#include "mirror/object_array-inl.h"
#include "object_utils.h"
#include "scoped_thread_state_change.h"
#include "stack.h"
#include "well_known_classes.h"

namespace art {

class ArgArray {
 public:
  explicit ArgArray(const char* shorty, uint32_t shorty_len)
      : shorty_(shorty), shorty_len_(shorty_len), num_bytes_(0) {
    size_t num_slots = shorty_len + 1;  // +1 in case of receiver.
    if (LIKELY((num_slots * 2) < kSmallArgArraySize)) {
      // We can trivially use the small arg array.
      arg_array_ = small_arg_array_;
    } else {
      // Analyze shorty to see if we need the large arg array.
      for (size_t i = 1; i < shorty_len; ++i) {
        char c = shorty[i];
        if (c == 'J' || c == 'D') {
          num_slots++;
        }
      }
      if (num_slots <= kSmallArgArraySize) {
        arg_array_ = small_arg_array_;
      } else {
        large_arg_array_.reset(new uint32_t[num_slots]);
        arg_array_ = large_arg_array_.get();
      }
    }
  }

  uint32_t* GetArray() {
    return arg_array_;
  }

  uint32_t GetNumBytes() {
    return num_bytes_;
  }

  void Append(uint32_t value) {
    arg_array_[num_bytes_ / 4] = value;
    num_bytes_ += 4;
  }

  void Append(mirror::Object* obj) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Append(StackReference<mirror::Object>::FromMirrorPtr(obj).AsVRegValue());
  }

  void AppendWide(uint64_t value) {
    // For ARM and MIPS portable, align wide values to 8 bytes (ArgArray starts at offset of 4).
#if defined(ART_USE_PORTABLE_COMPILER) && (defined(__arm__) || defined(__mips__))
    if (num_bytes_ % 8 == 0) {
      num_bytes_ += 4;
    }
#endif
    arg_array_[num_bytes_ / 4] = value;
    arg_array_[(num_bytes_ / 4) + 1] = value >> 32;
    num_bytes_ += 8;
  }

  void AppendFloat(float value) {
    jvalue jv;
    jv.f = value;
    Append(jv.i);
  }

  void AppendDouble(double value) {
    jvalue jv;
    jv.d = value;
    AppendWide(jv.j);
  }

  void BuildArgArray(const ScopedObjectAccess& soa, mirror::Object* receiver, va_list ap)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // Set receiver if non-null (method is not static)
    if (receiver != nullptr) {
      Append(receiver);
    }
    for (size_t i = 1; i < shorty_len_; ++i) {
      switch (shorty_[i]) {
        case 'Z':
        case 'B':
        case 'C':
        case 'S':
        case 'I':
          Append(va_arg(ap, jint));
          break;
        case 'F':
          AppendFloat(va_arg(ap, jdouble));
          break;
        case 'L':
          Append(soa.Decode<mirror::Object*>(va_arg(ap, jobject)));
          break;
        case 'D':
          AppendDouble(va_arg(ap, jdouble));
          break;
        case 'J':
          AppendWide(va_arg(ap, jlong));
          break;
#ifndef NDEBUG
        default:
          LOG(FATAL) << "Unexpected shorty character: " << shorty_[i];
#endif
      }
    }
  }

  void BuildArgArray(const ScopedObjectAccessUnchecked& soa, mirror::Object* receiver,
                     jvalue* args)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // Set receiver if non-null (method is not static)
    if (receiver != nullptr) {
      Append(receiver);
    }
    for (size_t i = 1, args_offset = 0; i < shorty_len_; ++i, ++args_offset) {
      switch (shorty_[i]) {
        case 'Z':
          Append(args[args_offset].z);
          break;
        case 'B':
          Append(args[args_offset].b);
          break;
        case 'C':
          Append(args[args_offset].c);
          break;
        case 'S':
          Append(args[args_offset].s);
          break;
        case 'I':
        case 'F':
          Append(args[args_offset].i);
          break;
        case 'L':
          Append(soa.Decode<mirror::Object*>(args[args_offset].l));
          break;
        case 'D':
        case 'J':
          AppendWide(args[args_offset].j);
          break;
#ifndef NDEBUG
        default:
          LOG(FATAL) << "Unexpected shorty character: " << shorty_[i];
#endif
      }
    }
  }

  void BuildArgArrayFromFrame(ShadowFrame* shadow_frame, uint32_t arg_offset)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // Set receiver if non-null (method is not static)
    size_t cur_arg = arg_offset;
    if (!shadow_frame->GetMethod()->IsStatic()) {
      Append(shadow_frame->GetVReg(cur_arg));
      cur_arg++;
    }
    for (size_t i = 1; i < shorty_len_; ++i) {
      switch (shorty_[i]) {
        case 'Z':
        case 'B':
        case 'C':
        case 'S':
        case 'I':
        case 'F':
        case 'L':
          Append(shadow_frame->GetVReg(cur_arg));
          cur_arg++;
          break;
        case 'D':
        case 'J':
          AppendWide(shadow_frame->GetVRegLong(cur_arg));
          cur_arg++;
          cur_arg++;
          break;
#ifndef NDEBUG
        default:
          LOG(FATAL) << "Unexpected shorty character: " << shorty_[i];
#endif
      }
    }
  }

  static void ThrowIllegalPrimitiveArgumentException(const char* expected,
                                                     const StringPiece& found_descriptor)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    ThrowIllegalArgumentException(nullptr,
        StringPrintf("Invalid primitive conversion from %s to %s", expected,
                     PrettyDescriptor(found_descriptor.as_string()).c_str()).c_str());
  }

  bool BuildArgArray(const ScopedObjectAccess& soa, mirror::Object* receiver,
                     mirror::ObjectArray<mirror::Object>* args, MethodHelper& mh)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    const DexFile::TypeList* classes = mh.GetParameterTypeList();
    // Set receiver if non-null (method is not static)
    if (receiver != nullptr) {
      Append(receiver);
    }
    for (size_t i = 1, args_offset = 0; i < shorty_len_; ++i, ++args_offset) {
      mirror::Object* arg = args->Get(args_offset);
      if (((shorty_[i] == 'L') && (arg != nullptr)) || ((arg == nullptr && shorty_[i] != 'L'))) {
        mirror::Class* dst_class =
            mh.GetClassFromTypeIdx(classes->GetTypeItem(args_offset).type_idx_);
        if (UNLIKELY(arg == nullptr || !arg->InstanceOf(dst_class))) {
          ThrowIllegalArgumentException(nullptr,
              StringPrintf("method %s argument %zd has type %s, got %s",
                  PrettyMethod(mh.GetMethod(), false).c_str(),
                  args_offset + 1,  // Humans don't count from 0.
                  PrettyDescriptor(dst_class).c_str(),
                  PrettyTypeOf(arg).c_str()).c_str());
          return false;
        }
      }

#define DO_FIRST_ARG(match_descriptor, get_fn, append) { \
          const StringPiece src_descriptor(arg != nullptr \
              ? ClassHelper(arg->GetClass<>()).GetDescriptor() \
              : "null"); \
          if (LIKELY(src_descriptor == match_descriptor)) { \
            mirror::ArtField* primitive_field = arg->GetClass()->GetIFields()->Get(0); \
            append(primitive_field-> get_fn(arg));

#define DO_ARG(match_descriptor, get_fn, append) \
          } else if (LIKELY(src_descriptor == match_descriptor)) { \
            mirror::ArtField* primitive_field = arg->GetClass()->GetIFields()->Get(0); \
            append(primitive_field-> get_fn(arg));

#define DO_FAIL(expected) \
          } else { \
            if (arg->GetClass<>()->IsPrimitive()) { \
              ThrowIllegalPrimitiveArgumentException(expected, src_descriptor); \
            } else { \
              ThrowIllegalArgumentException(nullptr, \
                  StringPrintf("method %s argument %zd has type %s, got %s", \
                      PrettyMethod(mh.GetMethod(), false).c_str(), \
                      args_offset + 1, \
                      expected, \
                      PrettyTypeOf(arg).c_str()).c_str()); \
            } \
            return false; \
          } }

      switch (shorty_[i]) {
        case 'L':
          Append(arg);
          break;
        case 'Z':
          DO_FIRST_ARG("Ljava/lang/Boolean;", GetBoolean, Append)
          DO_FAIL("boolean")
          break;
        case 'B':
          DO_FIRST_ARG("Ljava/lang/Byte;", GetByte, Append)
          DO_FAIL("byte")
          break;
        case 'C':
          DO_FIRST_ARG("Ljava/lang/Character;", GetChar, Append)
          DO_FAIL("char")
          break;
        case 'S':
          DO_FIRST_ARG("Ljava/lang/Short;", GetShort, Append)
          DO_ARG("Ljava/lang/Byte;", GetByte, Append)
          DO_FAIL("short")
          break;
        case 'I':
          DO_FIRST_ARG("Ljava/lang/Integer;", GetInt, Append)
          DO_ARG("Ljava/lang/Character;", GetChar, Append)
          DO_ARG("Ljava/lang/Short;", GetShort, Append)
          DO_ARG("Ljava/lang/Byte;", GetByte, Append)
          DO_FAIL("int")
          break;
        case 'J':
          DO_FIRST_ARG("Ljava/lang/Long;", GetLong, AppendWide)
          DO_ARG("Ljava/lang/Integer;", GetInt, AppendWide)
          DO_ARG("Ljava/lang/Character;", GetChar, AppendWide)
          DO_ARG("Ljava/lang/Short;", GetShort, AppendWide)
          DO_ARG("Ljava/lang/Byte;", GetByte, AppendWide)
          DO_FAIL("long")
          break;
        case 'F':
          DO_FIRST_ARG("Ljava/lang/Float;", GetFloat, AppendFloat)
          DO_ARG("Ljava/lang/Long;", GetLong, AppendFloat)
          DO_ARG("Ljava/lang/Integer;", GetInt, AppendFloat)
          DO_ARG("Ljava/lang/Character;", GetChar, AppendFloat)
          DO_ARG("Ljava/lang/Short;", GetShort, AppendFloat)
          DO_ARG("Ljava/lang/Byte;", GetByte, AppendFloat)
          DO_FAIL("float")
          break;
        case 'D':
          DO_FIRST_ARG("Ljava/lang/Double;", GetDouble, AppendDouble)
          DO_ARG("Ljava/lang/Float;", GetFloat, AppendDouble)
          DO_ARG("Ljava/lang/Long;", GetLong, AppendDouble)
          DO_ARG("Ljava/lang/Integer;", GetInt, AppendDouble)
          DO_ARG("Ljava/lang/Character;", GetChar, AppendDouble)
          DO_ARG("Ljava/lang/Short;", GetShort, AppendDouble)
          DO_ARG("Ljava/lang/Byte;", GetByte, AppendDouble)
          DO_FAIL("double")
          break;
#ifndef NDEBUG
        default:
          LOG(FATAL) << "Unexpected shorty character: " << shorty_[i];
#endif
      }
#undef DO_FIRST_ARG
#undef DO_ARG
#undef DO_FAIL
    }
    return true;
  }

 private:
  enum { kSmallArgArraySize = 16 };
  const char* const shorty_;
  const uint32_t shorty_len_;
  uint32_t num_bytes_;
  uint32_t* arg_array_;
  uint32_t small_arg_array_[kSmallArgArraySize];
  UniquePtr<uint32_t[]> large_arg_array_;
};

static void CheckMethodArguments(mirror::ArtMethod* m, uint32_t* args)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  const DexFile::TypeList* params = MethodHelper(m).GetParameterTypeList();
  if (params == nullptr) {
    return;  // No arguments so nothing to check.
  }
  uint32_t offset = 0;
  uint32_t num_params = params->Size();
  size_t error_count = 0;
  if (!m->IsStatic()) {
    offset = 1;
  }
  for (uint32_t i = 0; i < num_params; i++) {
    uint16_t type_idx = params->GetTypeItem(i).type_idx_;
    mirror::Class* param_type = MethodHelper(m).GetClassFromTypeIdx(type_idx);
    if (param_type == nullptr) {
      Thread* self = Thread::Current();
      CHECK(self->IsExceptionPending());
      LOG(ERROR) << "Internal error: unresolvable type for argument type in JNI invoke: "
          << MethodHelper(m).GetTypeDescriptorFromTypeIdx(type_idx) << "\n"
          << self->GetException(nullptr)->Dump();
      self->ClearException();
      ++error_count;
    } else if (!param_type->IsPrimitive()) {
      // TODO: check primitives are in range.
      mirror::Object* argument = reinterpret_cast<mirror::Object*>(args[i + offset]);
      if (argument != nullptr && !argument->InstanceOf(param_type)) {
        LOG(ERROR) << "JNI ERROR (app bug): attempt to pass an instance of "
                   << PrettyTypeOf(argument) << " as argument " << (i + 1)
                   << " to " << PrettyMethod(m);
        ++error_count;
      }
    } else if (param_type->IsPrimitiveLong() || param_type->IsPrimitiveDouble()) {
      offset++;
    }
  }
  if (error_count > 0) {
    // TODO: pass the JNI function name (such as "CallVoidMethodV") through so we can call JniAbort
    // with an argument.
    JniAbortF(nullptr, "bad arguments passed to %s (see above for details)",
              PrettyMethod(m).c_str());
  }
}

static mirror::ArtMethod* FindVirtualMethod(mirror::Object* receiver,
                                            mirror::ArtMethod* method)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return receiver->GetClass()->FindVirtualMethodForVirtualOrInterface(method);
}


static void InvokeWithArgArray(const ScopedObjectAccessUnchecked& soa, mirror::ArtMethod* method,
                               ArgArray* arg_array, JValue* result, const char* shorty)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  uint32_t* args = arg_array->GetArray();
  if (UNLIKELY(soa.Env()->check_jni)) {
    CheckMethodArguments(method, args);
  }
  method->Invoke(soa.Self(), args, arg_array->GetNumBytes(), result, shorty);
}

JValue InvokeWithVarArgs(const ScopedObjectAccess& soa, jobject obj, jmethodID mid, va_list args)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtMethod* method = soa.DecodeMethod(mid);
  mirror::Object* receiver = method->IsStatic() ? nullptr : soa.Decode<mirror::Object*>(obj);
  MethodHelper mh(method);
  JValue result;
  ArgArray arg_array(mh.GetShorty(), mh.GetShortyLength());
  arg_array.BuildArgArray(soa, receiver, args);
  InvokeWithArgArray(soa, method, &arg_array, &result, mh.GetShorty());
  return result;
}

JValue InvokeWithJValues(const ScopedObjectAccessUnchecked& soa, mirror::Object* receiver,
                         jmethodID mid, jvalue* args) {
  mirror::ArtMethod* method = soa.DecodeMethod(mid);
  MethodHelper mh(method);
  JValue result;
  ArgArray arg_array(mh.GetShorty(), mh.GetShortyLength());
  arg_array.BuildArgArray(soa, receiver, args);
  InvokeWithArgArray(soa, method, &arg_array, &result, mh.GetShorty());
  return result;
}

JValue InvokeVirtualOrInterfaceWithJValues(const ScopedObjectAccess& soa,
                                           mirror::Object* receiver, jmethodID mid, jvalue* args) {
  mirror::ArtMethod* method = FindVirtualMethod(receiver, soa.DecodeMethod(mid));
  MethodHelper mh(method);
  JValue result;
  ArgArray arg_array(mh.GetShorty(), mh.GetShortyLength());
  arg_array.BuildArgArray(soa, receiver, args);
  InvokeWithArgArray(soa, method, &arg_array, &result, mh.GetShorty());
  return result;
}

JValue InvokeVirtualOrInterfaceWithVarArgs(const ScopedObjectAccess& soa,
                                           jobject obj, jmethodID mid, va_list args) {
  mirror::Object* receiver = soa.Decode<mirror::Object*>(obj);
  mirror::ArtMethod* method = FindVirtualMethod(receiver, soa.DecodeMethod(mid));
  MethodHelper mh(method);
  JValue result;
  ArgArray arg_array(mh.GetShorty(), mh.GetShortyLength());
  arg_array.BuildArgArray(soa, receiver, args);
  InvokeWithArgArray(soa, method, &arg_array, &result, mh.GetShorty());
  return result;
}

void InvokeWithShadowFrame(Thread* self, ShadowFrame* shadow_frame, uint16_t arg_offset,
                           MethodHelper& mh, JValue* result) {
  ArgArray arg_array(mh.GetShorty(), mh.GetShortyLength());
  arg_array.BuildArgArrayFromFrame(shadow_frame, arg_offset);
  shadow_frame->GetMethod()->Invoke(self, arg_array.GetArray(), arg_array.GetNumBytes(), result,
                                    mh.GetShorty());
}

jobject InvokeMethod(const ScopedObjectAccess& soa, jobject javaMethod,
                     jobject javaReceiver, jobject javaArgs) {
  jmethodID mid = soa.Env()->FromReflectedMethod(javaMethod);
  mirror::ArtMethod* m = soa.DecodeMethod(mid);

  mirror::Class* declaring_class = m->GetDeclaringClass();
  if (UNLIKELY(!declaring_class->IsInitialized())) {
    SirtRef<mirror::Class> sirt_c(soa.Self(), declaring_class);
    if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(sirt_c, true, true)) {
      return nullptr;
    }
    declaring_class = sirt_c.get();
  }

  mirror::Object* receiver = nullptr;
  if (!m->IsStatic()) {
    // Check that the receiver is non-null and an instance of the field's declaring class.
    receiver = soa.Decode<mirror::Object*>(javaReceiver);
    if (!VerifyObjectIsClass(receiver, declaring_class)) {
      return NULL;
    }

    // Find the actual implementation of the virtual method.
    m = receiver->GetClass()->FindVirtualMethodForVirtualOrInterface(m);
  }

  // Get our arrays of arguments and their types, and check they're the same size.
  mirror::ObjectArray<mirror::Object>* objects =
      soa.Decode<mirror::ObjectArray<mirror::Object>*>(javaArgs);
  MethodHelper mh(m);
  const DexFile::TypeList* classes = mh.GetParameterTypeList();
  uint32_t classes_size = (classes == nullptr) ? 0 : classes->Size();
  uint32_t arg_count = (objects != nullptr) ? objects->GetLength() : 0;
  if (arg_count != classes_size) {
    ThrowIllegalArgumentException(NULL,
                                  StringPrintf("Wrong number of arguments; expected %d, got %d",
                                               classes_size, arg_count).c_str());
    return NULL;
  }

  // Invoke the method.
  JValue result;
  ArgArray arg_array(mh.GetShorty(), mh.GetShortyLength());
  if (!arg_array.BuildArgArray(soa, receiver, objects, mh)) {
    CHECK(soa.Self()->IsExceptionPending());
    return nullptr;
  }

  InvokeWithArgArray(soa, m, &arg_array, &result, mh.GetShorty());

  // Wrap any exception with "Ljava/lang/reflect/InvocationTargetException;" and return early.
  if (soa.Self()->IsExceptionPending()) {
    jthrowable th = soa.Env()->ExceptionOccurred();
    soa.Env()->ExceptionClear();
    jclass exception_class = soa.Env()->FindClass("java/lang/reflect/InvocationTargetException");
    jmethodID mid = soa.Env()->GetMethodID(exception_class, "<init>", "(Ljava/lang/Throwable;)V");
    jobject exception_instance = soa.Env()->NewObject(exception_class, mid, th);
    soa.Env()->Throw(reinterpret_cast<jthrowable>(exception_instance));
    return NULL;
  }

  // Box if necessary and return.
  return soa.AddLocalReference<jobject>(BoxPrimitive(mh.GetReturnType()->GetPrimitiveType(),
                                                     result));
}

bool VerifyObjectIsClass(mirror::Object* o, mirror::Class* c) {
  if (o == NULL) {
    ThrowNullPointerException(NULL, "null receiver");
    return false;
  } else if (!o->InstanceOf(c)) {
    std::string expected_class_name(PrettyDescriptor(c));
    std::string actual_class_name(PrettyTypeOf(o));
    ThrowIllegalArgumentException(NULL,
                                  StringPrintf("Expected receiver of type %s, but got %s",
                                               expected_class_name.c_str(),
                                               actual_class_name.c_str()).c_str());
    return false;
  }
  return true;
}

bool ConvertPrimitiveValue(const ThrowLocation* throw_location, bool unbox_for_result,
                           Primitive::Type srcType, Primitive::Type dstType,
                           const JValue& src, JValue& dst) {
  CHECK(srcType != Primitive::kPrimNot && dstType != Primitive::kPrimNot);
  switch (dstType) {
  case Primitive::kPrimBoolean:
    if (srcType == Primitive::kPrimBoolean) {
      dst.SetZ(src.GetZ());
      return true;
    }
    break;
  case Primitive::kPrimChar:
    if (srcType == Primitive::kPrimChar) {
      dst.SetC(src.GetC());
      return true;
    }
    break;
  case Primitive::kPrimByte:
    if (srcType == Primitive::kPrimByte) {
      dst.SetB(src.GetB());
      return true;
    }
    break;
  case Primitive::kPrimShort:
    if (srcType == Primitive::kPrimByte || srcType == Primitive::kPrimShort) {
      dst.SetS(src.GetI());
      return true;
    }
    break;
  case Primitive::kPrimInt:
    if (srcType == Primitive::kPrimByte || srcType == Primitive::kPrimChar ||
        srcType == Primitive::kPrimShort || srcType == Primitive::kPrimInt) {
      dst.SetI(src.GetI());
      return true;
    }
    break;
  case Primitive::kPrimLong:
    if (srcType == Primitive::kPrimByte || srcType == Primitive::kPrimChar ||
        srcType == Primitive::kPrimShort || srcType == Primitive::kPrimInt) {
      dst.SetJ(src.GetI());
      return true;
    } else if (srcType == Primitive::kPrimLong) {
      dst.SetJ(src.GetJ());
      return true;
    }
    break;
  case Primitive::kPrimFloat:
    if (srcType == Primitive::kPrimByte || srcType == Primitive::kPrimChar ||
        srcType == Primitive::kPrimShort || srcType == Primitive::kPrimInt) {
      dst.SetF(src.GetI());
      return true;
    } else if (srcType == Primitive::kPrimLong) {
      dst.SetF(src.GetJ());
      return true;
    } else if (srcType == Primitive::kPrimFloat) {
      dst.SetF(src.GetF());
      return true;
    }
    break;
  case Primitive::kPrimDouble:
    if (srcType == Primitive::kPrimByte || srcType == Primitive::kPrimChar ||
        srcType == Primitive::kPrimShort || srcType == Primitive::kPrimInt) {
      dst.SetD(src.GetI());
      return true;
    } else if (srcType == Primitive::kPrimLong) {
      dst.SetD(src.GetJ());
      return true;
    } else if (srcType == Primitive::kPrimFloat) {
      dst.SetD(src.GetF());
      return true;
    } else if (srcType == Primitive::kPrimDouble) {
      dst.SetJ(src.GetJ());
      return true;
    }
    break;
  default:
    break;
  }
  if (!unbox_for_result) {
    ThrowIllegalArgumentException(throw_location,
                                  StringPrintf("Invalid primitive conversion from %s to %s",
                                               PrettyDescriptor(srcType).c_str(),
                                               PrettyDescriptor(dstType).c_str()).c_str());
  } else {
    ThrowClassCastException(throw_location,
                            StringPrintf("Couldn't convert result of type %s to %s",
                                         PrettyDescriptor(srcType).c_str(),
                                         PrettyDescriptor(dstType).c_str()).c_str());
  }
  return false;
}

mirror::Object* BoxPrimitive(Primitive::Type src_class, const JValue& value) {
  if (src_class == Primitive::kPrimNot) {
    return value.GetL();
  }
  if (src_class == Primitive::kPrimVoid) {
    // There's no such thing as a void field, and void methods invoked via reflection return null.
    return nullptr;
  }

  jmethodID m = NULL;
  const char* shorty;
  switch (src_class) {
  case Primitive::kPrimBoolean:
    m = WellKnownClasses::java_lang_Boolean_valueOf;
    shorty = "LZ";
    break;
  case Primitive::kPrimByte:
    m = WellKnownClasses::java_lang_Byte_valueOf;
    shorty = "LB";
    break;
  case Primitive::kPrimChar:
    m = WellKnownClasses::java_lang_Character_valueOf;
    shorty = "LC";
    break;
  case Primitive::kPrimDouble:
    m = WellKnownClasses::java_lang_Double_valueOf;
    shorty = "LD";
    break;
  case Primitive::kPrimFloat:
    m = WellKnownClasses::java_lang_Float_valueOf;
    shorty = "LF";
    break;
  case Primitive::kPrimInt:
    m = WellKnownClasses::java_lang_Integer_valueOf;
    shorty = "LI";
    break;
  case Primitive::kPrimLong:
    m = WellKnownClasses::java_lang_Long_valueOf;
    shorty = "LJ";
    break;
  case Primitive::kPrimShort:
    m = WellKnownClasses::java_lang_Short_valueOf;
    shorty = "LS";
    break;
  default:
    LOG(FATAL) << static_cast<int>(src_class);
    shorty = nullptr;
  }

  ScopedObjectAccessUnchecked soa(Thread::Current());
  DCHECK_EQ(soa.Self()->GetState(), kRunnable);

  ArgArray arg_array(shorty, 2);
  JValue result;
  if (src_class == Primitive::kPrimDouble || src_class == Primitive::kPrimLong) {
    arg_array.AppendWide(value.GetJ());
  } else {
    arg_array.Append(value.GetI());
  }

  soa.DecodeMethod(m)->Invoke(soa.Self(), arg_array.GetArray(), arg_array.GetNumBytes(),
                              &result, shorty);
  return result.GetL();
}

static std::string UnboxingFailureKind(mirror::ArtMethod* m, int index, mirror::ArtField* f)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (m != NULL && index != -1) {
    ++index;  // Humans count from 1.
    return StringPrintf("method %s argument %d", PrettyMethod(m, false).c_str(), index);
  }
  if (f != NULL) {
    return "field " + PrettyField(f, false);
  }
  return "result";
}

static bool UnboxPrimitive(const ThrowLocation* throw_location, mirror::Object* o,
                           mirror::Class* dst_class, JValue& unboxed_value,
                           mirror::ArtMethod* m, int index, mirror::ArtField* f)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  bool unbox_for_result = (f == NULL) && (index == -1);
  if (!dst_class->IsPrimitive()) {
    if (UNLIKELY(o != NULL && !o->InstanceOf(dst_class))) {
      if (!unbox_for_result) {
        ThrowIllegalArgumentException(throw_location,
                                      StringPrintf("%s has type %s, got %s",
                                                   UnboxingFailureKind(m, index, f).c_str(),
                                                   PrettyDescriptor(dst_class).c_str(),
                                                   PrettyTypeOf(o).c_str()).c_str());
      } else {
        ThrowClassCastException(throw_location,
                                StringPrintf("Couldn't convert result of type %s to %s",
                                             PrettyTypeOf(o).c_str(),
                                             PrettyDescriptor(dst_class).c_str()).c_str());
      }
      return false;
    }
    unboxed_value.SetL(o);
    return true;
  }
  if (UNLIKELY(dst_class->GetPrimitiveType() == Primitive::kPrimVoid)) {
    ThrowIllegalArgumentException(throw_location,
                                  StringPrintf("Can't unbox %s to void",
                                               UnboxingFailureKind(m, index, f).c_str()).c_str());
    return false;
  }
  if (UNLIKELY(o == NULL)) {
    if (!unbox_for_result) {
      ThrowIllegalArgumentException(throw_location,
                                    StringPrintf("%s has type %s, got null",
                                                 UnboxingFailureKind(m, index, f).c_str(),
                                                 PrettyDescriptor(dst_class).c_str()).c_str());
    } else {
      ThrowNullPointerException(throw_location,
                                StringPrintf("Expected to unbox a '%s' primitive type but was returned null",
                                             PrettyDescriptor(dst_class).c_str()).c_str());
    }
    return false;
  }

  JValue boxed_value;
  const StringPiece src_descriptor(ClassHelper(o->GetClass()).GetDescriptor());
  mirror::Class* src_class = NULL;
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  mirror::ArtField* primitive_field = o->GetClass()->GetIFields()->Get(0);
  if (src_descriptor == "Ljava/lang/Boolean;") {
    src_class = class_linker->FindPrimitiveClass('Z');
    boxed_value.SetZ(primitive_field->GetBoolean(o));
  } else if (src_descriptor == "Ljava/lang/Byte;") {
    src_class = class_linker->FindPrimitiveClass('B');
    boxed_value.SetB(primitive_field->GetByte(o));
  } else if (src_descriptor == "Ljava/lang/Character;") {
    src_class = class_linker->FindPrimitiveClass('C');
    boxed_value.SetC(primitive_field->GetChar(o));
  } else if (src_descriptor == "Ljava/lang/Float;") {
    src_class = class_linker->FindPrimitiveClass('F');
    boxed_value.SetF(primitive_field->GetFloat(o));
  } else if (src_descriptor == "Ljava/lang/Double;") {
    src_class = class_linker->FindPrimitiveClass('D');
    boxed_value.SetD(primitive_field->GetDouble(o));
  } else if (src_descriptor == "Ljava/lang/Integer;") {
    src_class = class_linker->FindPrimitiveClass('I');
    boxed_value.SetI(primitive_field->GetInt(o));
  } else if (src_descriptor == "Ljava/lang/Long;") {
    src_class = class_linker->FindPrimitiveClass('J');
    boxed_value.SetJ(primitive_field->GetLong(o));
  } else if (src_descriptor == "Ljava/lang/Short;") {
    src_class = class_linker->FindPrimitiveClass('S');
    boxed_value.SetS(primitive_field->GetShort(o));
  } else {
    ThrowIllegalArgumentException(throw_location,
                                  StringPrintf("%s has type %s, got %s",
                                               UnboxingFailureKind(m, index, f).c_str(),
                                               PrettyDescriptor(dst_class).c_str(),
                                               PrettyDescriptor(src_descriptor.data()).c_str()).c_str());
    return false;
  }

  return ConvertPrimitiveValue(throw_location, unbox_for_result,
                               src_class->GetPrimitiveType(), dst_class->GetPrimitiveType(),
                               boxed_value, unboxed_value);
}

bool UnboxPrimitiveForArgument(mirror::Object* o, mirror::Class* dst_class, JValue& unboxed_value,
                               mirror::ArtMethod* m, size_t index) {
  CHECK(m != NULL);
  return UnboxPrimitive(NULL, o, dst_class, unboxed_value, m, index, NULL);
}

bool UnboxPrimitiveForField(mirror::Object* o, mirror::Class* dst_class, JValue& unboxed_value,
                            mirror::ArtField* f) {
  CHECK(f != NULL);
  return UnboxPrimitive(NULL, o, dst_class, unboxed_value, NULL, -1, f);
}

bool UnboxPrimitiveForResult(const ThrowLocation& throw_location, mirror::Object* o,
                             mirror::Class* dst_class, JValue& unboxed_value) {
  return UnboxPrimitive(&throw_location, o, dst_class, unboxed_value, NULL, -1, NULL);
}

}  // namespace art
