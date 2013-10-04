// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// This file contains implementations for the JNI FeSupport interface.

#include "service/fe-support.h"

#include <boost/scoped_ptr.hpp>

#include "codegen/llvm-codegen.h"
#include "common/init.h"
#include "common/logging.h"
#include "common/status.h"
#include "exprs/expr.h"
#include "runtime/exec-env.h"
#include "runtime/runtime-state.h"
#include "runtime/hdfs-fs-cache.h"
#include "runtime/lib-cache.h"
#include "runtime/client-cache.h"
#include "util/cpu-info.h"
#include "util/disk-info.h"
#include "util/dynamic-util.h"
#include "util/jni-util.h"
#include "util/logging.h"
#include "util/mem-info.h"
#include "util/symbols-util.h"
#include "rpc/thrift-util.h"
#include "rpc/thrift-server.h"
#include "util/debug-util.h"
#include "gen-cpp/Data_types.h"
#include "gen-cpp/Frontend_types.h"

using namespace impala;
using namespace std;
using namespace boost;
using namespace apache::thrift::server;

// Called from the FE when it explicitly loads libfesupport.so for tests.
// This creates the minimal state necessary to service the other JNI calls.
// This is not called when we first start up the BE.
extern "C"
JNIEXPORT void JNICALL
Java_com_cloudera_impala_service_FeSupport_NativeFeTestInit(
    JNIEnv* env, jclass caller_class) {
  DCHECK(ExecEnv::GetInstance() == NULL) << "This should only be called once from the FE";
  char* name = const_cast<char*>("FeSupport");
  InitCommonRuntime(1, &name, false);
  LlvmCodeGen::InitializeLlvm();
  new ExecEnv(); // This also caches it from the process.
}

// Requires JniUtil::Init() to have been called.
extern "C"
JNIEXPORT jbyteArray JNICALL
Java_com_cloudera_impala_service_FeSupport_NativeEvalConstExpr(
    JNIEnv* env, jclass caller_class, jbyteArray thrift_predicate_bytes,
    jbyteArray thrift_query_globals_bytes) {
  ObjectPool obj_pool;
  TExpr thrift_predicate;
  DeserializeThriftMsg(env, thrift_predicate_bytes, &thrift_predicate);
  TQueryGlobals query_globals;
  DeserializeThriftMsg(env, thrift_query_globals_bytes, &query_globals);
  RuntimeState state(query_globals.now_string, query_globals.user);
  jbyteArray result_bytes = NULL;
  JniLocalFrame jni_frame;
  Expr* e;
  THROW_IF_ERROR_RET(jni_frame.push(env), env, JniUtil::internal_exc_class(),
                     result_bytes);
  THROW_IF_ERROR_RET(Expr::CreateExprTree(&obj_pool, thrift_predicate, &e), env,
                     JniUtil::internal_exc_class(), result_bytes);
  THROW_IF_ERROR_RET(Expr::Prepare(e, &state, RowDescriptor()), env,
                     JniUtil::internal_exc_class(), result_bytes);

  TColumnValue val;
  e->GetValue(NULL, false, &val);
  THROW_IF_ERROR_RET(SerializeThriftMsg(env, &val, &result_bytes), env,
                     JniUtil::internal_exc_class(), result_bytes);
  return result_bytes;
}

// Does the symbol resolution, filling in the result in *result.
static void ResolveSymbolLookup(const TSymbolLookupParams params,
    const vector<ColumnType>& arg_types, TSymbolLookupResult* result) {
  ExecEnv* env = ExecEnv::GetInstance();
  DCHECK(env != NULL);
  DCHECK(params.fn_binary_type == TFunctionBinaryType::NATIVE ||
         params.fn_binary_type == TFunctionBinaryType::IR);
  bool is_shared_object = params.fn_binary_type == TFunctionBinaryType::NATIVE;

  string dummy_local_path;
  Status status =
      env->lib_cache()->GetLocalLibPath(env->fs_cache(), params.location,
          is_shared_object, &dummy_local_path);
  if (!status.ok()) {
    result->__set_result_code(TSymbolLookupResultCode::BINARY_NOT_FOUND);
    result->__set_error_msg(status.GetErrorMsg());
    return;
  }

  status = env->lib_cache()->CheckSymbolExists(
      env->fs_cache(), params.location, is_shared_object, params.symbol);
  if (status.ok()) {
    // The FE specified symbol exists, just use that.
    result->__set_result_code(TSymbolLookupResultCode::SYMBOL_FOUND);
    result->__set_symbol(params.symbol);
    // TODO: we can demangle the user symbol here and validate it against
    // params.arg_types. This would prevent someone from typing the wrong symbol
    // by accident. This requires more string parsing of the symbol.
    return;
  }

  // The input was already mangled and we couldn't find it, return the error.
  if (SymbolsUtil::IsMangled(params.symbol)) {
    result->__set_result_code(TSymbolLookupResultCode::SYMBOL_NOT_FOUND);
    result->__set_error_msg(status.GetErrorMsg());
    return;
  }

  // Mangle the user input and do another lookup.
  ColumnType ret_type(INVALID_TYPE);
  if (params.__isset.ret_arg_type) ret_type = ColumnType(params.ret_arg_type);
  string symbol = SymbolsUtil::MangleUserFunction(params.symbol,
      arg_types, params.has_var_args, params.__isset.ret_arg_type ? &ret_type : NULL);

  status = env->lib_cache()->CheckSymbolExists(
      env->fs_cache(), params.location, is_shared_object, symbol);
  if (!status.ok()) {
    result->__set_result_code(TSymbolLookupResultCode::SYMBOL_NOT_FOUND);
    result->__set_error_msg("Could not find symbol.");
    return;
  }

  // We were able to resolve the symbol.
  result->__set_result_code(TSymbolLookupResultCode::SYMBOL_FOUND);
  result->__set_symbol(symbol);
}

extern "C"
JNIEXPORT jbyteArray JNICALL
Java_com_cloudera_impala_service_FeSupport_NativeLookupSymbol(
    JNIEnv* env, jclass caller_class, jbyteArray thrift_struct) {
  TSymbolLookupParams lookup;
  DeserializeThriftMsg(env, thrift_struct, &lookup);

  vector<ColumnType> arg_types;
  for (int i = 0; i < lookup.arg_types.size(); ++i) {
    arg_types.push_back(ColumnType(lookup.arg_types[i]));
  }

  TSymbolLookupResult result;
  ResolveSymbolLookup(lookup, arg_types, &result);

  jbyteArray result_bytes = NULL;
  THROW_IF_ERROR_RET(SerializeThriftMsg(env, &result, &result_bytes), env,
                     JniUtil::internal_exc_class(), result_bytes);
  return result_bytes;
}

namespace impala {

static JNINativeMethod native_methods[] = {
  {
    (char*)"NativeFeTestInit", (char*)"()V",
    (void*)::Java_com_cloudera_impala_service_FeSupport_NativeFeTestInit
  },
  {
    (char*)"NativeEvalConstExpr", (char*)"([B[B)[B",
    (void*)::Java_com_cloudera_impala_service_FeSupport_NativeEvalConstExpr
  },
  {
    (char*)"NativeLookupSymbol", (char*)"([B)[B",
    (void*)::Java_com_cloudera_impala_service_FeSupport_NativeLookupSymbol
  },
};

void InitFeSupport() {
  JNIEnv* env = getJNIEnv();
  jclass native_backend_cl = env->FindClass("com/cloudera/impala/service/FeSupport");
  env->RegisterNatives(native_backend_cl, native_methods,
      sizeof(native_methods) / sizeof(native_methods[0]));
  EXIT_IF_EXC(env);
}

}
