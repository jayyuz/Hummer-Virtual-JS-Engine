#include <test.h>

EXTERN_C_START

static NAPIValue runWithCNullThis(NAPIEnv env, NAPICallbackInfo info)
{
    size_t argc = 1;
    NAPIValue argv[1];
    assert(napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) == NAPIOK);
    assert(argc == 1);
    NAPIValueType valueType;
    assert(napi_typeof(env, argv[0], &valueType) == NAPIOK);
    assert(valueType == NAPIFunction);
    assert(napi_call_function(env, nullptr, argv[0], 0, nullptr, nullptr) == NAPIOK);

    return nullptr;
}

static NAPIValue run(NAPIEnv env, NAPICallbackInfo info)
{
    size_t argc = 1;
    NAPIValue argv[1];
    assert(napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) == NAPIOK);
    assert(argc == 1);
    NAPIValueType valueType;
    assert(napi_typeof(env, argv[0], &valueType) == NAPIOK);
    assert(valueType == NAPIFunction);
    NAPIValue global;
    assert(napi_get_global(env, &global) == NAPIOK);
    assert(napi_call_function(env, global, argv[0], 0, nullptr, nullptr) == NAPIOK);

    return nullptr;
}

static NAPIValue runWithArgument(NAPIEnv env, NAPICallbackInfo info)
{
    size_t argc = 3;
    NAPIValue argv[3];
    assert(napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) == NAPIOK);
    assert(argc == 3);
    NAPIValueType valueType;
    assert(napi_typeof(env, argv[0], &valueType) == NAPIOK);
    assert(valueType == NAPIFunction);
    assert(napi_typeof(env, argv[1], &valueType) == NAPIOK);
    assert(valueType == NAPIString);
    assert(napi_typeof(env, argv[2], &valueType) == NAPIOK);
    assert(valueType == NAPIString);
    NAPIValue args[] = {argv[1], argv[2]};
    assert(napi_call_function(env, nullptr, argv[0], 2, args, nullptr) == NAPIOK);

    return nullptr;
}

EXTERN_C_END

TEST_F(Test, Callable)
{
    NAPIValue runWithUndefinedThisValue, runValue, runWithArgumentValue;
    ASSERT_EQ(napi_create_function(globalEnv, nullptr, -1, runWithCNullThis, globalEnv, &runWithUndefinedThisValue),
              NAPIOK);
    ASSERT_EQ(napi_create_function(globalEnv, nullptr, -1, run, globalEnv, &runValue), NAPIOK);
    ASSERT_EQ(napi_create_function(globalEnv, nullptr, -1, runWithArgument, globalEnv, &runWithArgumentValue), NAPIOK);
    NAPIValue stringValue;
    ASSERT_EQ(napi_create_string_utf8(globalEnv, "runWithCNullThis", -1, &stringValue), NAPIOK);
    ASSERT_EQ(napi_set_property(globalEnv, addonValue, stringValue, runWithUndefinedThisValue), NAPIOK);
    ASSERT_EQ(napi_create_string_utf8(globalEnv, "run", -1, &stringValue), NAPIOK);
    ASSERT_EQ(napi_set_property(globalEnv, addonValue, stringValue, runValue), NAPIOK);
    ASSERT_EQ(napi_create_string_utf8(globalEnv, "runWithArgument", -1, &stringValue), NAPIOK);
    ASSERT_EQ(napi_set_property(globalEnv, addonValue, stringValue, runWithArgumentValue), NAPIOK);
    ASSERT_EQ(NAPIRunScript(
                  globalEnv,
                  "(()=>{\"use strict\";var "
                  "l=!1;globalThis.addon.runWithCNullThis((function(){l=!0,globalThis.assert(this===globalThis)})),"
                  "globalThis.assert(l),l=!1,globalThis.addon.run((function(){l=!0,globalThis.assert(this===globalThis)"
                  "})),globalThis.assert(l),globalThis.addon.runWithArgument((function(){globalThis.assert(\"hello\"==="
                  "(arguments.length<=0?void 0:arguments[0])),globalThis.assert(\"world\"===(arguments.length<=1?void "
                  "0:arguments[1]))}),\"hello\",\"world\")})();",
                  "https://www.napi.com/callable.js", nullptr),
              NAPIOK);
}