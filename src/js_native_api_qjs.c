// 所有函数都可能抛出 NAPIInvalidArg NAPIOK
// 当返回值为 NAPIOK 时候，必须保证 result 可用，但是发生错误的时候，result 也可能会被修改，所以建议不要对 result 做
// NULL 初始化

#include <assert.h>
#include <napi/js_native_api.h>
//#include <quickjs-libc.h>
#include <cutils.h>
#include <quickjs.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

#define RETURN_STATUS_IF_FALSE(condition, status)                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            return status;                                                                                             \
        }                                                                                                              \
    } while (0)

#define CHECK_NAPI(expr)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        NAPIStatus status = expr;                                                                                      \
        if (status != NAPIOK)                                                                                          \
        {                                                                                                              \
            return status;                                                                                             \
        }                                                                                                              \
    } while (0)

#define CHECK_ARG(arg) RETURN_STATUS_IF_FALSE(arg, NAPIInvalidArg)

// JS_GetException 会转移所有权
// JS_Throw 会获取所有权
// 所以如果存在异常，先获取所有权，再交还所有权，最终所有权依旧在
// JSContext 中，QuickJS 目前 JS null 代表正常，未来考虑写成这样
// !JS_IsUndefined(exceptionValue) && !JS_IsNull(exceptionValue)
// NAPI_PREAMBLE 同时会检查 env->context
#define NAPI_PREAMBLE(env)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        CHECK_ARG(env);                                                                                                \
        CHECK_ARG((env)->context);                                                                                     \
        JSValue exceptionValue = JS_GetException((env)->context);                                                      \
        if (!JS_IsNull(exceptionValue))                                                                                \
        {                                                                                                              \
            JS_Throw((env)->context, exceptionValue);                                                                  \
                                                                                                                       \
            return NAPIPendingException;                                                                               \
        }                                                                                                              \
    } while (0)

struct Handle
{
    JSValue value;            // 64/128
    SLIST_ENTRY(Handle) node; // size_t
};

struct OpaqueNAPIHandleScope
{
    SLIST_ENTRY(OpaqueNAPIHandleScope) node; // size_t
    SLIST_HEAD(, Handle) handleList;         // size_t
};

// 1. external -> 不透明指针 + finalizer + 调用一个回调
// 2. Function -> JSValue data 数组 + finalizer
// 3. Constructor -> .[[prototype]] = new External()
// 4. Reference -> 引用计数 + setWeak/clearWeak -> __reference__

// typedef struct
// {
//     const char *errorMessage; // size_t
//     NAPIStatus errorCode;     // enum，有需要的话根据 iOS 或 Android 的 clang 情况做单独处理
// } NAPIExtendedErrorInfo;
struct OpaqueNAPIEnv
{
    JSContext *context;                                 // size_t
    LIST_HEAD(, OpaqueNAPIHandleScope) handleScopeList; // size_t
};

// 这个函数不会修改引用计数和所有权
// NAPIHandleScopeMismatch/NAPIMemoryError
static NAPIStatus addValueToHandleScope(NAPIEnv env, JSValue value, struct Handle **result)
{
    CHECK_ARG(env);
    CHECK_ARG(result);

    RETURN_STATUS_IF_FALSE(!LIST_EMPTY(&(*env).handleScopeList), NAPIHandleScopeMismatch);
    *result = malloc(sizeof(struct Handle));
    RETURN_STATUS_IF_FALSE(*result, NAPIMemoryError);
    (*result)->value = value;
    SLIST_INSERT_HEAD(&(LIST_FIRST(&(env)->handleScopeList))->handleList, *result, node);

    return NAPIOK;
}

static JSValueConst undefinedValue = JS_UNDEFINED;

NAPIStatus napi_get_undefined(NAPIEnv env, NAPIValue *result)
{
    CHECK_ARG(env);
    CHECK_ARG(result);

    *result = (NAPIValue)&undefinedValue;

    return NAPIOK;
}

static JSValueConst nullValue = JS_NULL;

NAPIStatus napi_get_null(NAPIEnv env, NAPIValue *result)
{
    CHECK_ARG(env);
    CHECK_ARG(result);

    *result = (NAPIValue)&nullValue;

    return NAPIOK;
}

// NAPIGenericFailure + addValueToHandleScope
NAPIStatus napi_get_global(NAPIEnv env, NAPIValue *result)
{
    CHECK_ARG(env);
    CHECK_ARG(result);

    CHECK_ARG(env->context);

    // JS_GetGlobalObject 返回已经引用计数 +1
    // 实际上 globalValue 可能为 JS_EXCEPTION
    JSValue globalValue = JS_GetGlobalObject(env->context);
    RETURN_STATUS_IF_FALSE(!JS_IsException(globalValue), NAPIGenericFailure);
    struct Handle *globalHandle;
    NAPIStatus status = addValueToHandleScope(env, globalValue, &globalHandle);
    if (status != NAPIOK)
    {
        JS_FreeValue(env->context, globalValue);

        return status;
    }
    *result = (NAPIValue)&globalHandle->value;

    return NAPIOK;
}

// + addValueToHandleScope
NAPIStatus napi_get_boolean(NAPIEnv env, bool value, NAPIValue *result)
{
    CHECK_ARG(env);
    CHECK_ARG(result);

    // JS_NewBool 忽略 JSContext
    JSValue jsValue = JS_NewBool(env->context, value);
    struct Handle *handle;
    CHECK_NAPI(addValueToHandleScope(env, jsValue, &handle));
    *result = (NAPIValue)&handle->value;

    return NAPIOK;
}

// + addValueToHandleScope
NAPIStatus napi_create_double(NAPIEnv env, double value, NAPIValue *result)
{
    CHECK_ARG(env);
    CHECK_ARG(result);

    JSValue jsValue = JS_NewFloat64(env->context, value);
    struct Handle *handle;
    CHECK_NAPI(addValueToHandleScope(env, jsValue, &handle));
    *result = (NAPIValue)&handle->value;

    return NAPIOK;
}

// + addValueToHandleScope
NAPIStatus napi_create_int32(NAPIEnv env, int32_t value, NAPIValue *result)
{
    CHECK_ARG(env);
    CHECK_ARG(result);

    JSValue jsValue = JS_NewInt32(env->context, value);
    struct Handle *handle;
    CHECK_NAPI(addValueToHandleScope(env, jsValue, &handle));
    *result = (NAPIValue)&handle->value;

    return NAPIOK;
}

// + addValueToHandleScope
NAPIStatus napi_create_uint32(NAPIEnv env, uint32_t value, NAPIValue *result)
{
    CHECK_ARG(env);
    CHECK_ARG(result);

    // uint32_t 需要用 int64_t 承载
    JSValue jsValue = JS_NewInt64(env->context, value);
    struct Handle *handle;
    CHECK_NAPI(addValueToHandleScope(env, jsValue, &handle));
    *result = (NAPIValue)&handle->value;

    return NAPIOK;
}

// + addValueToHandleScope
NAPIStatus napi_create_int64(NAPIEnv env, int64_t value, NAPIValue *result)
{
    CHECK_ARG(env);
    CHECK_ARG(result);

    JSValue jsValue = JS_NewInt64(env->context, value);
    struct Handle *handle;
    CHECK_NAPI(addValueToHandleScope(env, jsValue, &handle));
    *result = (NAPIValue)&handle->value;

    return NAPIOK;
}

// NAPIPendingException + addValueToHandleScope
NAPIStatus napi_create_string_utf8(NAPIEnv env, const char *str, size_t length, NAPIValue *result)
{
    NAPI_PREAMBLE(env);
    CHECK_ARG(result);

    if (length == NAPI_AUTO_LENGTH)
    {
        if (str)
        {
            length = strlen(str);
        }
        else
        {
            // str 依旧保持 NULL
            length = 0;
        }
    }
    // length == 0 的情况下会返回 ""
    JSValue stringValue = JS_NewStringLen(env->context, str, length);
    RETURN_STATUS_IF_FALSE(!JS_IsException(stringValue), NAPIPendingException);
    struct Handle *stringHandle;
    NAPIStatus status = addValueToHandleScope(env, stringValue, &stringHandle);
    if (status != NAPIOK)
    {
        JS_FreeValue(env->context, stringValue);

        return status;
    }
    *result = (NAPIValue)&stringHandle->value;

    return NAPIOK;
}

typedef struct
{
    NAPIEnv env; // size_t
    void *data;  // size_t
} BaseInfo;

typedef struct
{
    BaseInfo baseInfo;
    NAPICallback callback; // size_t
} FunctionInfo;

static JSClassID functionClassId;

struct OpaqueNAPICallbackInfo
{
    JSValueConst newTarget; // 128/64
    JSValueConst thisArg;   // 128/64
    JSValueConst *argv;     // 128/64
    void *data;             // size_t
    int argc;
};

static JSValue callAsFunction(JSContext *ctx, JSValueConst thisVal, int argc, JSValueConst *argv, int magic,
                              JSValue *funcData)
{
    // 固定 1 参数
    // JS_GetOpaque 不会抛出异常
    FunctionInfo *functionInfo = JS_GetOpaque(funcData[0], functionClassId);
    if (!functionInfo || !functionInfo->callback || !functionInfo->baseInfo.env)
    {
        assert(false);

        return undefinedValue;
    }
    struct OpaqueNAPICallbackInfo callbackInfo = {undefinedValue, thisVal, argv, functionInfo->baseInfo.data, argc};
    // napi_open_handle_scope 失败需要容错，这里需要初始化为 NULL 判断
    NAPIHandleScope handleScope = NULL;
    NAPIStatus status = napi_open_handle_scope(functionInfo->baseInfo.env, &handleScope);
    // 内存分配失败，由最外层做 HandleScope
    RETURN_STATUS_IF_FALSE(napi_open_handle_scope(functionInfo->baseInfo.env, &handleScope) == NAPIOK, undefinedValue);
    // 正常返回值应当是由 HandleScope 持有，所以需要引用计数 +1
    JSValue returnValue =
        JS_DupValue(ctx, *((JSValue *)functionInfo->callback(functionInfo->baseInfo.env, &callbackInfo)));
    // 直接忽略错误
    napi_close_handle_scope(functionInfo->baseInfo.env, handleScope);
    JSValue exceptionValue = JS_GetException(ctx);
    if (!JS_IsNull(exceptionValue))
    {
        JS_FreeValue(ctx, returnValue);

        // 一行代码不要搞复杂了
        return JS_Throw(ctx, exceptionValue);
    }

    return returnValue;
}

static void functionFinalize(NAPIEnv env, void *finalizeData, void *finalizeHint)
{
    free(finalizeHint);
}

// NAPIMemoryError/NAPIPendingException + addValueToHandleScope
NAPIStatus napi_create_function(NAPIEnv env, const char *utf8name, size_t length, NAPICallback callback, void *data,
                                NAPIValue *result)
{
    NAPI_PREAMBLE(env);
    CHECK_ARG(callback);
    CHECK_ARG(result);

    // malloc
    FunctionInfo *functionInfo = malloc(sizeof(FunctionInfo));
    RETURN_STATUS_IF_FALSE(functionInfo, NAPIMemoryError);
    functionInfo->baseInfo.env = env;
    functionInfo->baseInfo.data = data;
    functionInfo->callback = callback;

    // rc: 1
    JSValue dataValue = JS_NewObjectClass(env->context, (int)functionClassId);
    if (JS_IsException(dataValue))
    {
        free(functionInfo);

        return NAPIPendingException;
    }
    // functionInfo 生命周期被 JSValue 托管
    JS_SetOpaque(dataValue, functionInfo);

    // fakePrototypeValue 引用计数 +1 => rc: 2
    JSValue functionValue = JS_NewCFunctionData(env->context, callAsFunction, 0, 0, 1, &dataValue);
    // 转移所有权
    JS_FreeValue(env->context, dataValue);
    // 如果 functionValue 创建失败，fakePrototypeValue rc 不会被 +1，上面的引用计数 -1 => rc 为
    // 0，发生垃圾回收，FunctionInfo 结构体也被回收
    RETURN_STATUS_IF_FALSE(!JS_IsException(functionValue), NAPIPendingException);
    struct Handle *functionHandle;
    NAPIStatus status = addValueToHandleScope(env, functionValue, &functionHandle);
    if (status != NAPIOK)
    {
        // 由于 fakePrototypeValue 所有权被 functionValue 持有，所以只需要对 functionValue 做引用计数 -1
        JS_FreeValue(env->context, functionValue);

        return status;
    }
    *result = (NAPIValue)&functionHandle->value;

    return NAPIOK;
}

static JSClassID externalClassId;

NAPIStatus napi_typeof(NAPIEnv env, NAPIValue value, NAPIValueType *result)
{
    CHECK_ARG(env);
    CHECK_ARG(value);
    CHECK_ARG(result);

    CHECK_ARG(env->context);

    JSValue jsValue = *((JSValue *)value);
    if (JS_IsUndefined(jsValue))
    {
        *result = NAPIUndefined;
    }
    else if (JS_IsNull(jsValue))
    {
        *result = NAPINull;
    }
    else if (JS_IsNumber(jsValue))
    {
        *result = NAPINumber;
    }
    else if (JS_IsBool(jsValue))
    {
        *result = NAPIBoolean;
    }
    else if (JS_IsString(jsValue))
    {
        *result = NAPIString;
    }
    else if (JS_IsFunction(env->context, jsValue))
    {
        *result = NAPIFunction;
    }
    else if (JS_GetOpaque(jsValue, externalClassId))
    {
        // JS_GetOpaque 会检查 classId
        *result = NAPIExternal;
    }
    else if (JS_IsObject(jsValue))
    {
        *result = NAPIObject;
    }
    else
    {
        return NAPIInvalidArg;
    }

    return NAPIOK;
}

// NAPINumberExpected
NAPIStatus napi_get_value_double(NAPIEnv env, NAPIValue value, double *result)
{
    CHECK_ARG(env);
    CHECK_ARG(value);
    CHECK_ARG(result);

    JSValue jsValue = *((JSValue *)value);
    int tag = JS_VALUE_GET_TAG(jsValue);
    if (tag == JS_TAG_INT)
    {
        *result = JS_VALUE_GET_INT(jsValue);
    }
    else if (JS_TAG_IS_FLOAT64(tag))
    {
        *result = JS_VALUE_GET_FLOAT64(jsValue);
    }
    else
    {
        return NAPINumberExpected;
    }

    return NAPIOK;
}

// NAPINumberExpected
NAPIStatus napi_get_value_int32(NAPIEnv env, NAPIValue value, int32_t *result)
{
    CHECK_ARG(env);
    CHECK_ARG(value);
    CHECK_ARG(result);

    JSValue jsValue = *((JSValue *)value);
    int tag = JS_VALUE_GET_TAG(jsValue);
    if (tag == JS_TAG_INT)
    {
        *result = JS_VALUE_GET_INT(jsValue);
    }
    else if (JS_TAG_IS_FLOAT64(tag))
    {
        *result = JS_VALUE_GET_FLOAT64(jsValue);
    }
    else
    {
        return NAPINumberExpected;
    }

    return NAPIOK;
}

// NAPINumberExpected
NAPIStatus napi_get_value_uint32(NAPIEnv env, NAPIValue value, uint32_t *result)
{
    CHECK_ARG(env);
    CHECK_ARG(value);
    CHECK_ARG(result);

    JSValue jsValue = *((JSValue *)value);
    int tag = JS_VALUE_GET_TAG(jsValue);
    if (tag == JS_TAG_INT)
    {
        *result = JS_VALUE_GET_INT(jsValue);
    }
    else if (JS_TAG_IS_FLOAT64(tag))
    {
        *result = JS_VALUE_GET_FLOAT64(jsValue);
    }
    else
    {
        return NAPINumberExpected;
    }

    return NAPIOK;
}

// NAPINumberExpected
NAPIStatus napi_get_value_int64(NAPIEnv env, NAPIValue value, int64_t *result)
{
    CHECK_ARG(env);
    CHECK_ARG(value);
    CHECK_ARG(result);

    JSValue jsValue = *((JSValue *)value);
    int tag = JS_VALUE_GET_TAG(jsValue);
    if (tag == JS_TAG_INT)
    {
        *result = JS_VALUE_GET_INT(jsValue);
    }
    else if (JS_TAG_IS_FLOAT64(tag))
    {
        *result = JS_VALUE_GET_FLOAT64(jsValue);
    }
    else
    {
        return NAPINumberExpected;
    }

    return NAPIOK;
}

// NAPIBooleanExpected
NAPIStatus napi_get_value_bool(NAPIEnv env, NAPIValue value, bool *result)
{
    CHECK_ARG(env);
    CHECK_ARG(value);
    CHECK_ARG(result);

    RETURN_STATUS_IF_FALSE(JS_IsBool(*((JSValue *)value)), NAPIBooleanExpected);
    *result = JS_VALUE_GET_BOOL(*((JSValue *)value));

    return NAPIOK;
}

static bool getUTF8CharacterBytesLength(uint8_t codePoint, uint8_t *bytesToWrite)
{
    if (!bytesToWrite)
    {
        return false;
    }
    if (codePoint >> 7 == 0)
    {
        // 0xxxxxxx
        *bytesToWrite = 1;
    }
    else if (codePoint >> 5 == 6)
    {
        // 110xxxxx
        *bytesToWrite = 2;
    }
    else if (codePoint >> 4 == 0xe)
    {
        // 1110xxxx
        *bytesToWrite = 3;
    }
    else if (codePoint >> 3 == 0x1e)
    {
        // 11110xxx
        *bytesToWrite = 4;
    }
    else if (codePoint >> 2 == 0x3e)
    {
        // 111110xx
        *bytesToWrite = 5;
    }
    else if (codePoint >> 1 == 0x7e)
    {
        // 1111110x
        *bytesToWrite = 6;
    }
    else
    {
        return false;
    }

    return true;
}

// NAPIStringExpected/NAPIPendingException/NAPIGenericFailure
NAPIStatus napi_get_value_string_utf8(NAPIEnv env, NAPIValue value, char *buf, size_t bufSize, size_t *result)
{
    NAPI_PREAMBLE(env);
    CHECK_ARG(value);

    RETURN_STATUS_IF_FALSE(JS_IsString(*((JSValue *)value)), NAPIStringExpected);
    if (buf && !bufSize)
    {
        if (result)
        {
            *result = 0;
        }

        return NAPIOK;
    }
    if (bufSize == 1 && buf)
    {
        buf[0] = '\0';
        if (result)
        {
            *result = 0;
        }

        return NAPIOK;
    }
    size_t length;
    // JS_ToCStringLen 长度不包括 \0
    const char *cString = JS_ToCStringLen(env->context, &length, *((JSValue *)value));
    // JS_ToCStringLen 返回 (NULL, 0) 表示发生异常
    RETURN_STATUS_IF_FALSE(cString || length, NAPIPendingException);
    if (!buf)
    {
        JS_FreeCString(env->context, cString);
        CHECK_ARG(result);
        *result = length;

        return NAPIOK;
    }
    // bufSize >= 2 -> bufSize - 1 >= 1
    if (length <= bufSize - 1)
    {
        // memmove 函数将复制 n 字节。如果 n 为零，它将不起任何作用。
        memmove(buf, cString, length);
        JS_FreeCString(env->context, cString);
        buf[length] = '\0';
        if (result)
        {
            *result = length;
        }

        return NAPIOK;
    }
    // length > bufSize - 1 >= 1 -> length > 1
    // 截断，留 [bufSize - 1] 位置给 \0
    memmove(buf, cString, bufSize - 1);
    // 指向 UTF-8 串最后一个字节
    size_t initialIndex = bufSize - 2;
    size_t index = initialIndex;
    uint8_t bytesToWrite = 0;
    // 指向第一个字节的时候也应当继续判断
    while (index >= 0)
    {
        uint8_t codePoint = cString[index];
        if (codePoint >> 6 == 2 && index != 0)
        {
            --index;
            continue;
        }
        JS_FreeCString(env->context, cString);
        if (codePoint >> 6 == 2)
        {
            assert(false);

            return NAPIGenericFailure;
        }
        else if (codePoint >> 7 == 0 || codePoint >> 6 == 3)
        {
            // 0xxxxxxx 11xxxxxx
            if (!getUTF8CharacterBytesLength(codePoint, &bytesToWrite))
            {
                assert(false);

                return NAPIGenericFailure;
            }
            break;
        }
        else
        {
            assert(false);

            return NAPIGenericFailure;
        }
    }
    RETURN_STATUS_IF_FALSE(bytesToWrite, NAPIGenericFailure);
    // getUTF8CharacterBytesLength 返回 bytesToWrite >= 1
    // index + bytesToWrite - 1 < bufSize - 2 -> index + bytesToWrite < bufSize - 1
    if (index + bytesToWrite - 1 < initialIndex)
    {
        buf[index + bytesToWrite] = '\0';
        if (result)
        {
            *result = index + bytesToWrite;
        }
    }
    else if (index + bytesToWrite - 1 > initialIndex)
    {
        buf[index] = '\0';
        if (result)
        {
            *result = index;
        }
    }
    else
    {
        buf[bufSize - 1] = '\0';
        if (result)
        {
            *result = bufSize - 1;
        }
    }

    return NAPIOK;
}

// NAPIPendingException + napi_get_boolean
NAPIStatus napi_coerce_to_bool(NAPIEnv env, NAPIValue value, NAPIValue *result)
{
    NAPI_PREAMBLE(env);
    CHECK_ARG(value);
    CHECK_ARG(result);

    int boolStatus = JS_ToBool(env->context, *((JSValue *)value));
    RETURN_STATUS_IF_FALSE(boolStatus != -1, NAPIPendingException);
    CHECK_NAPI(napi_get_boolean(env, boolStatus, result));

    return NAPIOK;
}

// NAPIPendingException + napi_create_double
NAPIStatus napi_coerce_to_number(NAPIEnv env, NAPIValue value, NAPIValue *result)
{
    NAPI_PREAMBLE(env);
    CHECK_ARG(value);
    CHECK_ARG(result);

    double doubleValue;
    // JS_ToFloat64 只有 -1 0 两种
    int floatStatus = JS_ToFloat64(env->context, &doubleValue, *((JSValue *)value));
    RETURN_STATUS_IF_FALSE(floatStatus != -1, NAPIPendingException);
    CHECK_NAPI(napi_create_double(env, doubleValue, result));

    return NAPIOK;
}

// NAPIPendingException + addValueToHandleScope
NAPIStatus napi_coerce_to_string(NAPIEnv env, NAPIValue value, NAPIValue *result)
{
    NAPI_PREAMBLE(env);
    CHECK_ARG(value);
    CHECK_ARG(result);

    JSValue stringValue = JS_ToString(env->context, *((JSValue *)value));
    RETURN_STATUS_IF_FALSE(!JS_IsException(stringValue), NAPIPendingException);
    struct Handle *stringHandle;
    NAPIStatus status = addValueToHandleScope(env, stringValue, &stringHandle);
    if (status != NAPIOK)
    {
        JS_FreeValue(env->context, stringValue);

        return status;
    }
    *result = (NAPIValue)&stringHandle->value;

    return NAPIOK;
}

// NAPIPendingException/NAPIGenericFailure
NAPIStatus napi_set_property(NAPIEnv env, NAPIValue object, NAPIValue key, NAPIValue value)
{
    NAPI_PREAMBLE(env);
    CHECK_ARG(object);
    CHECK_ARG(key);
    CHECK_ARG(value);

    JSAtom atom = JS_ValueToAtom(env->context, *((JSValue *)key));
    RETURN_STATUS_IF_FALSE(atom != JS_ATOM_NULL, NAPIPendingException);
    int status =
        JS_SetProperty(env->context, *((JSValue *)object), atom, JS_DupValue(env->context, *((JSValue *)value)));
    JS_FreeAtom(env->context, atom);
    RETURN_STATUS_IF_FALSE(status != -1, NAPIPendingException);
    RETURN_STATUS_IF_FALSE(status, NAPIGenericFailure);

    return NAPIOK;
}

// NAPIPendingException
NAPIStatus napi_has_property(NAPIEnv env, NAPIValue object, NAPIValue key, bool *result)
{
    NAPI_PREAMBLE(env);
    CHECK_ARG(object);
    CHECK_ARG(key);
    CHECK_ARG(result);

    JSAtom atom = JS_ValueToAtom(env->context, *((JSValue *)key));
    RETURN_STATUS_IF_FALSE(atom != JS_ATOM_NULL, NAPIPendingException);
    int status = JS_HasProperty(env->context, *((JSValue *)object), atom);
    JS_FreeAtom(env->context, atom);
    RETURN_STATUS_IF_FALSE(status != -1, NAPIPendingException);
    *result = status;

    return NAPIOK;
}

// NAPIPendingException + addValueToHandleScope
NAPIStatus napi_get_property(NAPIEnv env, NAPIValue object, NAPIValue key, NAPIValue *result)
{
    NAPI_PREAMBLE(env);
    CHECK_ARG(object);
    CHECK_ARG(key);
    CHECK_ARG(result);

    JSAtom atom = JS_ValueToAtom(env->context, *((JSValue *)key));
    RETURN_STATUS_IF_FALSE(atom != JS_ATOM_NULL, NAPIPendingException);
    JSValue value = JS_GetProperty(env->context, *((JSValue *)object), atom);
    JS_FreeAtom(env->context, atom);
    RETURN_STATUS_IF_FALSE(!JS_IsException(value), NAPIPendingException);
    struct Handle *handle;
    NAPIStatus status = addValueToHandleScope(env, value, &handle);
    if (status != NAPIOK)
    {
        JS_FreeValue(env->context, value);

        return status;
    }
    *result = (NAPIValue)&handle->value;

    return NAPIOK;
}

// NAPIPendingException
NAPIStatus napi_delete_property(NAPIEnv env, NAPIValue object, NAPIValue key, bool *result)
{
    NAPI_PREAMBLE(env);
    CHECK_ARG(object);
    CHECK_ARG(key);

    JSAtom atom = JS_ValueToAtom(env->context, *((JSValue *)key));
    RETURN_STATUS_IF_FALSE(atom != JS_ATOM_NULL, NAPIPendingException);
    // 不抛出异常
    int status = JS_DeleteProperty(env->context, *((JSValue *)object), atom, 0);
    JS_FreeAtom(env->context, atom);
    RETURN_STATUS_IF_FALSE(status != -1, NAPIPendingException);
    if (result)
    {
        *result = status;
    }

    return NAPIOK;
}

// NAPIMemoryError/NAPIPendingException + addValueToHandleScope
NAPIStatus napi_call_function(NAPIEnv env, NAPIValue thisValue, NAPIValue func, size_t argc, const NAPIValue *argv,
                              NAPIValue *result)
{
    NAPI_PREAMBLE(env);
    CHECK_ARG(thisValue);
    CHECK_ARG(func);

    JSValue *internalArgv = NULL;
    if (argc > 0)
    {
        CHECK_ARG(argv);
        internalArgv = malloc(sizeof(JSValue) * argc);
        RETURN_STATUS_IF_FALSE(internalArgv, NAPIMemoryError);
        for (size_t i = 0; i < argc; ++i)
        {
            internalArgv[i] = *((JSValue *)argv[i]);
        }
    }

    // JS_Call 返回值带所有权
    JSValue returnValue = JS_Call(env->context, *((JSValue *)func), *((JSValue *)thisValue), (int)argc, internalArgv);
    free(internalArgv);
    RETURN_STATUS_IF_FALSE(!JS_IsException(returnValue), NAPIPendingException);
    if (result)
    {
        struct Handle *handle;
        NAPIStatus status = addValueToHandleScope(env, returnValue, &handle);
        if (status != NAPIOK)
        {
            JS_FreeValue(env->context, returnValue);

            return status;
        }
        *result = (NAPIValue)&handle->value;
    }

    return NAPIOK;
}

// NAPIPendingException/NAPIMemoryError + addValueToHandleScope
NAPIStatus napi_new_instance(NAPIEnv env, NAPIValue constructor, size_t argc, const NAPIValue *argv, NAPIValue *result)
{
    NAPI_PREAMBLE(env);
    CHECK_ARG(constructor);
    CHECK_ARG(result);

    JSValue *internalArgv = NULL;
    if (argc > 0)
    {
        CHECK_ARG(argv);
        internalArgv = malloc(sizeof(JSValue) * argc);
        RETURN_STATUS_IF_FALSE(internalArgv, NAPIMemoryError);
        for (size_t i = 0; i < argc; ++i)
        {
            internalArgv[i] = *((JSValue *)argv[i]);
        }
    }

    JSValue returnValue = JS_CallConstructor(env->context, *((JSValue *)constructor), (int)argc, internalArgv);
    free(internalArgv);
    RETURN_STATUS_IF_FALSE(!JS_IsException(returnValue), NAPIPendingException);
    struct Handle *handle;
    NAPIStatus status = addValueToHandleScope(env, returnValue, &handle);
    if (status != NAPIOK)
    {
        JS_FreeValue(env->context, returnValue);

        return status;
    }
    *result = (NAPIValue)&handle->value;

    return NAPIOK;
}

// NAPIPendingException
NAPIStatus napi_instanceof(NAPIEnv env, NAPIValue object, NAPIValue constructor, bool *result)
{
    NAPI_PREAMBLE(env);
    CHECK_ARG(object);
    CHECK_ARG(constructor);
    CHECK_ARG(result);

    int status = JS_IsInstanceOf(env->context, *((JSValue *)object), *((JSValue *)constructor));
    RETURN_STATUS_IF_FALSE(status != -1, NAPIPendingException);
    *result = status;

    return NAPIOK;
}

NAPIStatus napi_get_cb_info(NAPIEnv env, NAPICallbackInfo callbackInfo, size_t *argc, NAPIValue *argv,
                            NAPIValue *thisArg, void **data)
{
    CHECK_ARG(env);
    CHECK_ARG(callbackInfo);

    if (argv)
    {
        CHECK_ARG(argc);
        size_t i = 0;
        size_t min = *argc > callbackInfo->argc ? callbackInfo->argc : *argc;
        for (; i < min; ++i)
        {
            argv[i] = (NAPIValue)&callbackInfo->argv[i];
        }
        if (i < *argc)
        {
            for (; i < *argc; ++i)
            {
                argv[i] = (NAPIValue)&undefinedValue;
            }
        }
    }
    if (argc)
    {
        *argc = callbackInfo->argc;
    }
    if (thisArg)
    {
        *thisArg = (NAPIValue)&callbackInfo->thisArg;
    }
    if (data)
    {
        *data = callbackInfo->data;
    }

    return NAPIOK;
}

NAPIStatus napi_get_new_target(NAPIEnv env, NAPICallbackInfo callbackInfo, NAPIValue *result)
{
    CHECK_ARG(env);
    CHECK_ARG(callbackInfo);
    CHECK_ARG(result);

    *result = (NAPIValue)&callbackInfo->newTarget;

    return NAPIOK;
}

typedef struct
{
    BaseInfo baseInfo;
    void *finalizeHint;            // size_t
    NAPIFinalize finalizeCallback; // size_t
} ExternalInfo;

// NAPIMemoryError/NAPIPendingException + addValueToHandleScope
NAPIStatus napi_create_external(NAPIEnv env, void *data, NAPIFinalize finalizeCB, void *finalizeHint, NAPIValue *result)
{
    NAPI_PREAMBLE(env);
    CHECK_ARG(result);

    ExternalInfo *externalInfo = malloc(sizeof(ExternalInfo));
    RETURN_STATUS_IF_FALSE(externalInfo, NAPIMemoryError);
    externalInfo->baseInfo.env = env;
    externalInfo->baseInfo.data = data;
    externalInfo->finalizeHint = finalizeHint;
    externalInfo->finalizeCallback = NULL;
    JSValue object = JS_NewObjectClass(env->context, (int)externalClassId);
    if (JS_IsException(object))
    {
        free(externalInfo);

        return NAPIPendingException;
    }
    JS_SetOpaque(object, externalInfo);
    struct Handle *handle;
    NAPIStatus status = addValueToHandleScope(env, object, &handle);
    if (status != NAPIOK)
    {
        JS_FreeValue(env->context, object);

        return status;
    }
    *result = (NAPIValue)&handle->value;
    // 不能先设置回调，万一出错，业务方也会收到回调
    externalInfo->finalizeCallback = finalizeCB;

    return NAPIOK;
}

NAPIStatus napi_get_value_external(NAPIEnv env, NAPIValue value, void **result)
{
    CHECK_ARG(env);
    CHECK_ARG(value);
    CHECK_ARG(result);

    ExternalInfo *externalInfo = JS_GetOpaque(*((JSValue *)value), externalClassId);
    *result = externalInfo ? externalInfo->baseInfo.data : NULL;

    return NAPIOK;
}

struct OpaqueNAPIRef
{
    JSValue value;                  // 64/128
    LIST_ENTRY(OpaqueNAPIRef) node; // size_t
    uint8_t referenceCount;         // 8
};

typedef struct
{
    LIST_HEAD(, OpaqueNAPIRef) referenceList;
} ReferenceInfo;

void referenceFinalize(NAPIEnv env, void *finalizeData, void *finalizeHint)
{
    if (!finalizeData)
    {
        assert(false);

        return;
    }
    ReferenceInfo *referenceInfo = finalizeData;
    NAPIRef reference;
    LIST_FOREACH(reference, &referenceInfo->referenceList, node)
    {
        reference->value = undefinedValue;
    }
    free(referenceInfo);
}

static const char *const referenceString = "__reference__";

// NAPIMemoryError/NAPIGenericFailure + napi_create_string_utf8 + napi_get_property + napi_typeof + napi_create_external
// + napi_get_value_external + napi_set_property
static NAPIStatus setWeak(NAPIEnv env, NAPIValue value, NAPIRef ref)
{
    CHECK_ARG(env);
    CHECK_ARG(value);
    CHECK_ARG(ref);

    NAPIValue stringValue;
    CHECK_NAPI(napi_create_string_utf8(env, referenceString, -1, &stringValue));
    NAPIValue referenceValue;
    CHECK_NAPI(napi_get_property(env, value, stringValue, &referenceValue));
    NAPIValueType valueType;
    CHECK_NAPI(napi_typeof(env, referenceValue, &valueType));
    RETURN_STATUS_IF_FALSE(valueType == NAPIUndefined || valueType == NAPIExternal, NAPIGenericFailure);
    if (valueType == NAPIUndefined)
    {
        ReferenceInfo *referenceInfo = malloc(sizeof(ReferenceInfo));
        RETURN_STATUS_IF_FALSE(referenceInfo, NAPIMemoryError);
        LIST_INIT(&referenceInfo->referenceList);
        NAPIStatus status = napi_create_external(env, referenceInfo, referenceFinalize, NULL, &referenceValue);
        if (status != NAPIOK)
        {
            free(referenceInfo);

            return status;
        }
    }
    ReferenceInfo *referenceInfo;
    CHECK_NAPI(napi_get_value_external(env, referenceValue, (void **)&referenceInfo));
    if (!referenceInfo)
    {
        assert(false);

        return NAPIGenericFailure;
    }
    if (valueType == NAPIUndefined)
    {
        CHECK_NAPI(napi_set_property(env, value, stringValue, referenceValue));
    }
    LIST_INSERT_HEAD(&referenceInfo->referenceList, ref, node);

    return NAPIOK;
}

// NAPIMemoryError + setWeak
NAPIStatus napi_create_reference(NAPIEnv env, NAPIValue value, uint32_t initialRefCount, NAPIRef *result)
{
    CHECK_ARG(env);
    CHECK_ARG(value);
    CHECK_ARG(result);

    CHECK_ARG(env->context);

    *result = malloc(sizeof(struct OpaqueNAPIRef));
    RETURN_STATUS_IF_FALSE(*result, NAPIMemoryError);
    if (!JS_IsObject(*((JSValue *)value)) && !initialRefCount)
    {
        (*result)->referenceCount = 0;
        (*result)->value = undefinedValue;

        return NAPIOK;
    }
    (*result)->value = *((JSValue *)value);
    (*result)->referenceCount = initialRefCount;
    if (!JS_IsObject(*((JSValue *)value)))
    {
        return NAPIOK;
    }
    if (initialRefCount)
    {
        (*result)->value = JS_DupValue(env->context, (*result)->value);

        return NAPIOK;
    }
    // setWeak
    NAPIStatus status = setWeak(env, value, *result);
    if (status != NAPIOK)
    {
        free(*result);

        return status;
    }

    return NAPIOK;
}

// NAPIGenericFailure + napi_create_string_utf8 + napi_get_property + napi_get_value_external + napi_delete_property
static NAPIStatus clearWeak(NAPIEnv env, NAPIRef ref)
{
    CHECK_ARG(env);
    CHECK_ARG(ref);

    NAPIValue stringValue;
    CHECK_NAPI(napi_create_string_utf8(env, referenceString, -1, &stringValue));
    NAPIValue externalValue;
    CHECK_NAPI(napi_get_property(env, (NAPIValue)&ref->value, stringValue, &externalValue));
    ReferenceInfo *referenceInfo;
    CHECK_NAPI(napi_get_value_external(env, externalValue, (void **)&referenceInfo));
    if (!referenceInfo)
    {
        assert(false);

        return NAPIGenericFailure;
    }
    if (!LIST_EMPTY(&referenceInfo->referenceList) && LIST_FIRST(&referenceInfo->referenceList) == ref &&
        !LIST_NEXT(ref, node))
    {
        bool deleteResult;
        CHECK_NAPI(napi_delete_property(env, (NAPIValue)&ref->value, stringValue, &deleteResult));
        RETURN_STATUS_IF_FALSE(deleteResult, NAPIGenericFailure);
    }
    LIST_REMOVE(ref, node);

    return NAPIOK;
}

// NAPIGenericFailure + clearWeak
NAPIStatus napi_delete_reference(NAPIEnv env, NAPIRef ref)
{
    CHECK_ARG(env);
    CHECK_ARG(ref);

    CHECK_ARG(env->context);

    if (!JS_IsObject(ref->value))
    {
        free(ref);

        return NAPIOK;
    }
    if (ref->referenceCount)
    {
        JS_FreeValue(env->context, ref->value);
        free(ref);

        return NAPIOK;
    }
    // 已经被 GC
    if (JS_IsUndefined(ref->value))
    {
        free(ref);

        return NAPIOK;
    }
    CHECK_NAPI(clearWeak(env, ref));
    free(ref);

    return NAPIOK;
}

// NAPIGenericFailure + clearWeak
NAPIStatus napi_reference_ref(NAPIEnv env, NAPIRef ref, uint32_t *result)
{
    CHECK_ARG(env);
    CHECK_ARG(ref);

    CHECK_ARG(env->context);

    // !ref->referenceCount && JS_IsUndefined(ref->value) 已经被 GC
    RETURN_STATUS_IF_FALSE(ref->referenceCount || !JS_IsUndefined(ref->value), NAPIGenericFailure);

    if (!ref->referenceCount)
    {
        if (JS_IsObject(ref->value))
        {
            CHECK_NAPI(clearWeak(env, ref));
        }
        ref->value = JS_DupValue(env->context, ref->value);
    }
    uint8_t count = ++ref->referenceCount;
    if (result)
    {
        *result = count;
    }

    return NAPIOK;
}

// NAPIGenericFailure + setWeak
NAPIStatus napi_reference_unref(NAPIEnv env, NAPIRef ref, uint32_t *result)
{
    CHECK_ARG(env);
    CHECK_ARG(ref);

    CHECK_ARG(env->context);

    RETURN_STATUS_IF_FALSE(ref->referenceCount, NAPIGenericFailure);

    if (ref->referenceCount == 1)
    {
        if (JS_IsObject(ref->value))
        {
            CHECK_NAPI(setWeak(env, (NAPIValue)&ref->value, ref));
            JS_FreeValue(env->context, ref->value);
        }
        else
        {
            JS_FreeValue(env->context, ref->value);
            ref->value = undefinedValue;
        }
    }
    uint8_t count = --ref->referenceCount;
    if (result)
    {
        *result = count;
    }

    return NAPIOK;
}

NAPIStatus napi_get_reference_value(NAPIEnv env, NAPIRef ref, NAPIValue *result)
{
    CHECK_ARG(env);
    CHECK_ARG(ref);
    CHECK_ARG(result);

    if (!ref->referenceCount && JS_IsUndefined(ref->value))
    {
        *result = NULL;
    }
    else
    {
        *result = (NAPIValue)&ref->value;
    }

    return NAPIOK;
}

// NAPIMemoryError
NAPIStatus napi_open_handle_scope(NAPIEnv env, NAPIHandleScope *result)
{
    CHECK_ARG(env);
    CHECK_ARG(result);

    *result = malloc(sizeof(struct OpaqueNAPIHandleScope));
    RETURN_STATUS_IF_FALSE(*result, NAPIMemoryError);
    SLIST_INIT(&(*result)->handleList);
    SLIST_INSERT_HEAD(&env->handleScopeList, *result, node);

    return NAPIOK;
}

// NAPIHandleScopeMismatch
NAPIStatus napi_close_handle_scope(NAPIEnv env, NAPIHandleScope scope)
{
    CHECK_ARG(env);
    CHECK_ARG(scope);

    // JS_FreeValue 不能传入 NULL
    CHECK_ARG(env->context);

    RETURN_STATUS_IF_FALSE(SLIST_FIRST(&env->handleScopeList) == scope, NAPIHandleScopeMismatch);
    struct Handle *handle, *tempHandle;
    SLIST_FOREACH_SAFE(handle, &scope->handleList, node, tempHandle)
    {
        JS_FreeValue(env->context, handle->value);
        free(handle);
    }
    SLIST_REMOVE_HEAD(&env->handleScopeList, node);
    free(scope);

    return NAPIOK;
}

struct OpaqueNAPIEscapableHandleScope
{
    struct OpaqueNAPIHandleScope handleScope;
    bool escapeCalled;
};

// NAPIMemoryError
NAPIStatus napi_open_escapable_handle_scope(NAPIEnv env, NAPIEscapableHandleScope *result)
{
    CHECK_ARG(env);
    CHECK_ARG(result);

    *result = malloc(sizeof(struct OpaqueNAPIEscapableHandleScope));
    RETURN_STATUS_IF_FALSE(*result, NAPIMemoryError);
    (*result)->escapeCalled = false;
    SLIST_INIT(&(*result)->handleScope.handleList);
    SLIST_INSERT_HEAD(&env->handleScopeList, &((*result)->handleScope), node);

    return NAPIOK;
}

// NAPIHandleScopeMismatch
NAPIStatus napi_close_escapable_handle_scope(NAPIEnv env, NAPIEscapableHandleScope scope)
{
    CHECK_ARG(env);
    CHECK_ARG(scope);

    // JS_FreeValue 不能传入 NULL
    CHECK_ARG(env->context);

    RETURN_STATUS_IF_FALSE(SLIST_FIRST(&env->handleScopeList) == &scope->handleScope, NAPIHandleScopeMismatch);
    struct Handle *handle, *tempHandle;
    SLIST_FOREACH_SAFE(handle, &(&scope->handleScope)->handleList, node, tempHandle)
    {
        JS_FreeValue(env->context, handle->value);
        free(handle);
    }
    SLIST_REMOVE_HEAD(&env->handleScopeList, node);
    free(scope);

    return NAPIOK;
}

// NAPIMemoryError/NAPIEscapeCalledTwice/NAPIHandleScopeMismatch
NAPIStatus napi_escape_handle(NAPIEnv env, NAPIEscapableHandleScope scope, NAPIValue escapee, NAPIValue *result)
{
    CHECK_ARG(env);
    CHECK_ARG(scope);
    CHECK_ARG(escapee);
    CHECK_ARG(result);

    RETURN_STATUS_IF_FALSE(!scope->escapeCalled, NAPIEscapeCalledTwice);

    NAPIHandleScope handleScope = SLIST_NEXT(&scope->handleScope, node);
    RETURN_STATUS_IF_FALSE(handleScope, NAPIHandleScopeMismatch);
    struct Handle *handle = malloc(sizeof(struct Handle));
    RETURN_STATUS_IF_FALSE(*result, NAPIMemoryError);
    scope->escapeCalled = true;
    handle->value = *((JSValue *)escapee);
    SLIST_INSERT_HEAD(&handleScope->handleList, handle, node);
    *result = (NAPIValue)&handle->value;

    return NAPIOK;
}

NAPIStatus napi_throw(NAPIEnv env, NAPIValue error)
{
    NAPI_PREAMBLE(env);
    CHECK_ARG(error);

    JS_Throw(env->context, *((JSValue *)error));

    return NAPIOK;
}

// + addValueToHandleScope
NAPIStatus napi_get_and_clear_last_exception(NAPIEnv env, NAPIValue *result)
{
    CHECK_ARG(env);
    CHECK_ARG(result);

    // JS_GetException
    CHECK_ARG(env->context);

    JSValue exceptionValue = JS_GetException(env->context);
    if (JS_IsNull(exceptionValue))
    {
        exceptionValue = JS_UNDEFINED;
    }
    struct Handle *exceptionHandle;
    NAPIStatus status = addValueToHandleScope(env, exceptionValue, &exceptionHandle);
    if (status != NAPIOK)
    {
        JS_FreeValue(env->context, exceptionValue);

        return status;
    }
    *result = (NAPIValue)&exceptionHandle->value;

    return NAPIOK;
}

// NAPIPendingException + addValueToHandleScope
NAPIStatus NAPIRunScript(NAPIEnv env, const char *script, const char *sourceUrl, NAPIValue *result)
{
    NAPI_PREAMBLE(env);

    // script 传入 NULL 会崩
    // utf8SourceUrl 传入 NULL 会导致崩溃，JS_NewAtom 调用 strlen 不能传入 NULL
    if (!sourceUrl)
    {
        sourceUrl = "";
    }
    JSValue returnValue = JS_Eval(env->context, script, script ? strlen(script) : 0, sourceUrl, JS_EVAL_TYPE_GLOBAL);
    for (int err = 1; err > 0;)
    {
        JSContext *context;
        err = JS_ExecutePendingJob(JS_GetRuntime(env->context), &context);
        if (err == -1)
        {
            // 正常情况下 JS_ExecutePendingJob 返回 -1
            // 代表引擎内部异常，比如内存分配失败等，未来如果真产生了 JS
            // 异常，也直接吃掉
            JSValue inlineExceptionValue = JS_GetException(context);
            JS_FreeValue(context, inlineExceptionValue);
        }
    }
    RETURN_STATUS_IF_FALSE(!JS_IsException(returnValue), NAPIPendingException);
    struct Handle *returnHandle;
    NAPIStatus status = addValueToHandleScope(env, returnValue, &returnHandle);
    if (status != NAPIOK)
    {
        JS_FreeValue(env->context, returnValue);

        return status;
    }
    *result = (NAPIValue)&returnHandle->value;

    return NAPIOK;
}

static uint8_t contextCount = 0;

static void functionFinalizer(JSRuntime *rt, JSValue val)
{
    FunctionInfo *functionInfo = JS_GetOpaque(val, functionClassId);
    free(functionInfo);
}

static void externalFinalizer(JSRuntime *rt, JSValue val)
{
    ExternalInfo *externalInfo = JS_GetOpaque(val, functionClassId);
    if (externalInfo && externalInfo->finalizeCallback)
    {
        externalInfo->finalizeCallback(externalInfo->baseInfo.env, externalInfo->baseInfo.data,
                                       externalInfo->finalizeHint);
    }
    free(externalInfo);
}

static JSRuntime *runtime = NULL;

typedef struct
{
    FunctionInfo functionInfo;
    JSClassID classId; // uint32_t
} ConstructorInfo;

static JSClassID constructorClassId;

static JSValue callAsConstructor(JSContext *ctx, JSValueConst newTarget, int argc, JSValueConst *argv)
{
    JSValue prototypeValue = JS_GetPrototype(ctx, newTarget);
    if (JS_IsException(prototypeValue))
    {
        return prototypeValue;
    }
    ConstructorInfo *constructorInfo = JS_GetOpaque(prototypeValue, constructorClassId);
    JS_FreeValue(ctx, prototypeValue);
    if (!constructorInfo || !constructorInfo->functionInfo.baseInfo.env || !constructorInfo->functionInfo.callback)
    {
        assert(false);

        return JS_UNDEFINED;
    }
    JSValue thisValue = JS_NewObjectClass(ctx, (int)constructorInfo->classId);
    if (JS_IsException(thisValue))
    {
        return thisValue;
    }
    struct OpaqueNAPICallbackInfo callbackInfo = {newTarget, thisValue, argv,
                                                  constructorInfo->functionInfo.baseInfo.data, argc};
    NAPIHandleScope handleScope;
    NAPIStatus status = napi_open_handle_scope(constructorInfo->functionInfo.baseInfo.env, &handleScope);
    if (status != NAPIOK)
    {
        assert(false);
        JS_FreeValue(ctx, thisValue);

        return JS_UNDEFINED;
    }
    JSValue returnValue = JS_DupValue(ctx, *((JSValue *)constructorInfo->functionInfo.callback(
                                               constructorInfo->functionInfo.baseInfo.env, &callbackInfo)));
    JS_FreeValue(ctx, thisValue);
    JSValue exceptionValue = JS_GetException(ctx);
    status = napi_close_handle_scope(constructorInfo->functionInfo.baseInfo.env, handleScope);
    if (status != NAPIOK)
    {
        assert(false);
        JS_FreeValue(ctx, returnValue);

        return JS_UNDEFINED;
    }
    if (!JS_IsNull(exceptionValue))
    {
        JS_Throw(ctx, exceptionValue);
        JS_FreeValue(ctx, returnValue);

        return JS_EXCEPTION;
    }

    return returnValue;
}

// NAPIMemoryError/NAPIPendingException + addValueToHandleScope
NAPIStatus NAPIDefineClass(NAPIEnv env, const char *utf8name, size_t length, NAPICallback constructor, void *data,
                           NAPIValue *result)
{
    NAPI_PREAMBLE(env);
    CHECK_ARG(constructor);
    CHECK_ARG(result);

    RETURN_STATUS_IF_FALSE(length == NAPI_AUTO_LENGTH, NAPIInvalidArg);
    RETURN_STATUS_IF_FALSE(runtime, NAPIInvalidArg);

    ConstructorInfo *constructorInfo = malloc(sizeof(ConstructorInfo));
    RETURN_STATUS_IF_FALSE(constructorInfo, NAPIMemoryError);
    constructorInfo->functionInfo.baseInfo.env = env;
    constructorInfo->functionInfo.baseInfo.data = data;
    constructorInfo->functionInfo.callback = constructor;
    JS_NewClassID(&constructorInfo->classId);
    JSClassDef classDef = {
        utf8name ?: "",
    };
    int status = JS_NewClass(runtime, constructorInfo->classId, &classDef);
    if (status == -1)
    {
        free(constructorInfo);

        return NAPIPendingException;
    }
    JSValue prototype = JS_NewObjectClass(env->context, (int)constructorClassId);
    if (JS_IsException(prototype))
    {
        free(constructorInfo);

        return NAPIPendingException;
    }
    JS_SetOpaque(prototype, constructorInfo);
    // JS_NewCFunction2 会正确处理 utf8name 为空情况
    JSValue constructorValue = JS_NewCFunction2(env->context, callAsConstructor, utf8name, 0, JS_CFUNC_constructor, 0);
    if (JS_IsException(constructorValue))
    {
        // prototype 会自己 free ConstructorInfo 结构体
        JS_FreeValue(env->context, prototype);

        return NAPIPendingException;
    }
    struct Handle *handle;
    NAPIStatus addStatus = addValueToHandleScope(env, constructorValue, &handle);
    if (addStatus != NAPIOK)
    {
        JS_FreeValue(env->context, constructorValue);
        JS_FreeValue(env->context, prototype);

        return status;
    }
    *result = (NAPIValue)&handle->value;
    // .prototype .constructor
    // 会自动引用计数 +1
    JS_SetConstructor(env->context, constructorValue, prototype);
    // context -> class_proto
    // 转移所有权
    JS_SetClassProto(env->context, constructorInfo->classId, prototype);

    return NAPIOK;
}

// NAPIGenericFailure/NAPIMemoryError
NAPIStatus NAPICreateEnv(NAPIEnv *env)
{
    if (!env)
    {
        return NAPIInvalidArg;
    }
    if ((runtime && !contextCount) || (!runtime && contextCount))
    {
        assert(false);

        return NAPIGenericFailure;
    }
    *env = malloc(sizeof(struct OpaqueNAPIEnv));
    if (!*env)
    {
        return NAPIMemoryError;
    }
    if (!runtime)
    {
        runtime = JS_NewRuntime();
        if (!runtime)
        {
            free(*env);

            return NAPIMemoryError;
        }
        // 一定成功
        JS_NewClassID(&constructorClassId);
        JS_NewClassID(&functionClassId);
        JS_NewClassID(&externalClassId);

        JSClassDef classDef = {"External", externalFinalizer};
        int status = JS_NewClass(runtime, externalClassId, &classDef);
        if (status == -1)
        {
            JS_FreeRuntime(runtime);
            free(*env);
            runtime = NULL;

            return NAPIMemoryError;
        }

        classDef.class_name = "ExternalFunction";
        classDef.finalizer = functionFinalizer;
        status = JS_NewClass(runtime, functionClassId, &classDef);
        if (status == -1)
        {
            JS_FreeRuntime(runtime);
            free(*env);
            runtime = NULL;

            return NAPIMemoryError;
        }

        classDef.class_name = "ExternalConstructor";
        classDef.finalizer = functionFinalizer;
        status = JS_NewClass(runtime, constructorClassId, &classDef);
        if (status == -1)
        {
            JS_FreeRuntime(runtime);
            free(*env);
            runtime = NULL;

            return NAPIMemoryError;
        }
    }
    JSContext *context = JS_NewContext(runtime);
    if (!context)
    {
        free(*env);
        JS_FreeRuntime(runtime);
        runtime = NULL;

        return NAPIMemoryError;
    }
    JSValue prototype = JS_NewObject(context);
    if (JS_IsException(prototype))
    {
        JS_FreeContext(context);
        JS_FreeRuntime(runtime);
        free(*env);
        runtime = NULL;

        return NAPIGenericFailure;
    }
    JS_SetClassProto(context, externalClassId, prototype);
    const char *string = "(function () { return Symbol(\"reference\") })();";
    (*env)->referenceSymbolValue =
        JS_Eval(context, string, strlen(string), "https://n-api.com/qjs_reference_symbol.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException((*env)->referenceSymbolValue))
    {
        JS_FreeContext(context);
        JS_FreeRuntime(runtime);
        free(*env);
        runtime = NULL;

        return NAPIGenericFailure;
    }
    prototype = JS_NewObject(context);
    if (JS_IsException(prototype))
    {
        JS_FreeContext(context);
        JS_FreeRuntime(runtime);
        free(*env);
        runtime = NULL;

        return NAPIGenericFailure;
    }
    JS_SetClassProto(context, constructorClassId, prototype);
    contextCount += 1;
    (*env)->context = context;
    SLIST_INIT(&(*env)->handleScopeList);

    return NAPIOK;
}

NAPIStatus NAPIFreeEnv(NAPIEnv env)
{
    CHECK_ARG(env);

    NAPIHandleScope handleScope, tempHandleScope;
    SLIST_FOREACH_SAFE(handleScope, &(*env).handleScopeList, node, tempHandleScope)
    {
        if (napi_close_handle_scope(env, handleScope) != NAPIOK)
        {
            assert(false);
        }
    }
    JS_FreeValue(env->context, env->referenceSymbolValue);
    JS_FreeContext(env->context);
    if (--contextCount == 0 && runtime)
    {
        // virtualMachine 不能为 NULL
        JS_FreeRuntime(runtime);
        runtime = NULL;
    }
    free(env);

    return NAPIOK;
}