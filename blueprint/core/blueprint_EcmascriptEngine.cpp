/*
  ==============================================================================

    blueprint_EcmascriptEngine.cpp
    Created: 24 Oct 2019 3:08:39pm

  ==============================================================================
*/


namespace blueprint
{

    namespace detail
    {

        static void fatalErrorHandler (void* udata, const char* msg)
        {
            (void) udata; // Ignored in this case, silence warning
            throw EcmascriptEngine::FatalError(msg);
        }

        static void safeCall(duk_context* ctx, const int numArgs)
        {
            if (duk_pcall(ctx, numArgs) != DUK_EXEC_SUCCESS)
            {
                const juce::String stack = duk_safe_to_stacktrace(ctx, -1);
                const juce::String msg = duk_safe_to_string(ctx, -1);

                throw EcmascriptEngine::Error(msg, stack);
            }
        }

        static void safeEvalString(duk_context* ctx, const juce::String& s)
        {
            if (duk_peval_string(ctx, s.toRawUTF8()) != DUK_EXEC_SUCCESS)
            {
                const juce::String stack = duk_safe_to_stacktrace(ctx, -1);
                const juce::String msg = duk_safe_to_string(ctx, -1);

                throw EcmascriptEngine::Error(msg, stack);
            }
        }

        static void safeCompileFile(duk_context* ctx, const juce::File& file)
        {
            auto name = file.getFileName();
            auto body = file.loadFileAsString();

            // Push the js filename to be compiled/evaluated
            duk_push_string(ctx, name.toRawUTF8());

            if (duk_pcompile_string_filename(ctx, DUK_COMPILE_EVAL, body.toRawUTF8()) != DUK_EXEC_SUCCESS)
            {
                const juce::String stack = duk_safe_to_stacktrace(ctx, -1);
                const juce::String msg = duk_safe_to_string(ctx, -1);

                throw EcmascriptEngine::Error(msg, stack);
            }
       }

    }

    //==============================================================================
    EcmascriptEngine::EcmascriptEngine()
    {
        // Allocate a new js heap
        ctx = duk_create_heap(NULL, NULL, NULL, NULL, detail::fatalErrorHandler);

        // Add console.log support
        duk_console_init(ctx, DUK_CONSOLE_FLUSH);
    }

    EcmascriptEngine::~EcmascriptEngine()
    {
        duk_destroy_heap(ctx);
    }

    //==============================================================================
    juce::var EcmascriptEngine::evaluate (const juce::String& code)
    {
        jassert(code.isNotEmpty());

        try {
            detail::safeEvalString(ctx, code);
        } catch (Error const& err) {
            reset();
            throw err;
        }

        auto result = readVarFromDukStack(ctx, -1);
        duk_pop(ctx);

        return result;
    }

    juce::var EcmascriptEngine::evaluate (const juce::File& code)
    {
        jassert(code.existsAsFile());
        jassert(code.loadFileAsString().isNotEmpty());

        try {
            detail::safeCompileFile(ctx, code);
            detail::safeCall(ctx, 0);
        } catch (Error const& err) {
            reset();
            throw err;
        }

        // Collect the return value
        auto result = readVarFromDukStack(ctx, -1);
        duk_pop(ctx);

        return result;
    }

    //==============================================================================
    void EcmascriptEngine::registerNativeMethod (const juce::String& name, juce::var::NativeFunction fn)
    {
        registerNativeProperty(name, juce::var(fn));
    }

    void EcmascriptEngine::registerNativeMethod (const juce::String& target, const juce::String& name, juce::var::NativeFunction fn)
    {
        registerNativeProperty(target, name, juce::var(fn));
    }

    //==============================================================================
    void EcmascriptEngine::registerNativeProperty (const juce::String& name, const juce::var& value)
    {
        duk_push_global_object(ctx);
        pushVarToDukStack(ctx, value);
        duk_put_prop_string(ctx, -2, name.toRawUTF8());
    }

    void EcmascriptEngine::registerNativeProperty (const juce::String& target, const juce::String& name, const juce::var& value)
    {
        try {
            detail::safeEvalString(ctx, target);
        } catch (Error const& err) {
            reset();
            throw err;
        }

        // Then assign the property
        pushVarToDukStack(ctx, value);
        duk_put_prop_string(ctx, -2, name.toRawUTF8());
    }

    //==============================================================================
    juce::var EcmascriptEngine::invoke (const juce::String& name, const std::vector<juce::var>& vargs)
    {
        try {
            detail::safeEvalString(ctx, name);

            if (!duk_is_function(ctx, -1)) {
                throw Error("Invocation failed, target is not a function.");
            }

            // Push the args to the duktape stack
            const auto nargs = static_cast<duk_idx_t>(vargs.size());
            duk_require_stack_top(ctx, nargs);

            for (auto& p : vargs)
                pushVarToDukStack(ctx, p);

            // Invocation
            detail::safeCall(ctx, nargs);
        } catch (Error const& err) {
            reset();
            throw err;
        }

        // Collect the return value
        auto result = readVarFromDukStack(ctx, -1);
        duk_pop(ctx);

        // Here, we know that we have potentially just pushed temporaries
        // to the stack. Duktape will eventually get around to garbage collection,
        // but specifically where we have LambdaHelper allocations for these
        // temporaries, we want to keep our release pool fairly small. For that
        // reason, we manually invoke a garbage collection pass right away.
        // TODO: Revisit with lightfuncs
        // duk_gc(ctx, 0);

        return result;
    }

    void EcmascriptEngine::reset()
    {
        // Clear out the stack so we can re-register native functions
        // after we clear out the lambda release pool etc.
        while (duk_get_top(ctx))
            duk_remove(ctx, duk_get_top_index(ctx));

        // Clear the LambdaHelper release pool as duktape does not call object
        // finalizers in the event of an evaluation error or duk_pcall failure.
        lambdaReleasePool.clear();
    }

    //==============================================================================
    void EcmascriptEngine::debuggerAttach()
    {
        duk_trans_socket_init();
        duk_trans_socket_waitconn();

        duk_debugger_attach(ctx,
            duk_trans_socket_read_cb,
            duk_trans_socket_write_cb,
            duk_trans_socket_peek_cb,
            duk_trans_socket_read_flush_cb,
            duk_trans_socket_write_flush_cb,
            NULL,
            [](duk_context* ctx, void* data)
            {
                duk_trans_socket_finish();

                auto engine = static_cast<EcmascriptEngine*>(data);
                engine->stopTimer();
            },
            this);

        // Start timer for duk_debugger_cooperate calls
        startTimer(200);
    }

    void EcmascriptEngine::debuggerDetach()
    {
        duk_debugger_detach(ctx);
    }

    //==============================================================================
    void EcmascriptEngine::timerCallback()
    {
        duk_debugger_cooperate(ctx);
    }

    //==============================================================================
    EcmascriptEngine::LambdaHelper::LambdaHelper(juce::var::NativeFunction fn, uint32_t _id)
        : callback(std::move(fn)), id(_id) {}

    duk_ret_t EcmascriptEngine::LambdaHelper::invokeFromDukContext (duk_context* ctx)
    {
        // First we have to retrieve the actual function pointer and our engine pointer
        // See: https://duktape.org/guide.html#hidden-symbol-properties
        duk_push_current_function(ctx);
        duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("LambdaHelperPtr"));

        LambdaHelper* helper = static_cast<LambdaHelper*>(duk_get_pointer(ctx, -1));
        duk_pop(ctx);

        // Then the engine...
        duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("EnginePtr"));
        EcmascriptEngine* engine = static_cast<EcmascriptEngine*>(duk_get_pointer(ctx, -1));

        // Pop back both the pointer and the "current function"
        duk_pop_2(ctx);

        // Now we can collect our args
        std::vector<juce::var> args;
        int nargs = duk_get_top(ctx);

        for (int i = 0; i < nargs; ++i)
            args.push_back(engine->readVarFromDukStack(ctx, i));

        // Now we can invoke the user method with its arguments
        auto result = std::invoke(helper->callback, juce::var::NativeFunctionArgs(
            juce::var(),
            args.data(),
            static_cast<int>(args.size())
        ));

        // For an undefined result, return 0 to notify the duktape interpreter
        if (result.isUndefined())
            return 0;

        // Otherwise, push the result to the stack and tell duktape
        engine->pushVarToDukStack(ctx, result);
        return 1;
   }

    duk_ret_t EcmascriptEngine::LambdaHelper::callbackFinalizer (duk_context* ctx)
    {
        // First we have to retrieve the actual function pointer and our engine pointer
        // See: https://duktape.org/guide.html#hidden-symbol-properties
        // And: https://duktape.org/api.html#duk_set_finalizer
        // In this case our function is at index 0.
        duk_require_function(ctx, 0);
        duk_get_prop_string(ctx, 0, DUK_HIDDEN_SYMBOL("LambdaHelperPtr"));

        LambdaHelper* helper = static_cast<LambdaHelper*>(duk_get_pointer(ctx, -1));
        duk_pop(ctx);

        // Then the engine...
        duk_get_prop_string(ctx, 0, DUK_HIDDEN_SYMBOL("EnginePtr"));
        EcmascriptEngine* engine = static_cast<EcmascriptEngine*>(duk_get_pointer(ctx, -1));

        // Pop back both the pointer and the "current function"
        duk_pop_2(ctx);

        // Clean up our lambda helper
        engine->removeLambdaHelper(helper);
        return 0;
    }

    //==============================================================================
    void EcmascriptEngine::removeLambdaHelper (LambdaHelper* helper)
    {
        lambdaReleasePool.erase(helper->id);
    }

    void EcmascriptEngine::pushVarToDukStack (duk_context* ctx, const juce::var& v)
    {
        if (v.isVoid() || v.isUndefined())
            return duk_push_undefined(ctx);
        if (v.isBool())
            return duk_push_boolean(ctx, (bool) v);
        if (v.isInt() || v.isInt64())
            return duk_push_int(ctx, (int) v);
        if (v.isDouble())
            return duk_push_number(ctx, (double) v);
        if (v.isString())
            return (void) duk_push_string(ctx, v.toString().toRawUTF8());
        if (v.isArray())
        {
            duk_idx_t arr_idx = duk_push_array(ctx);
            int i = 0;

            for (auto& e : *(v.getArray()))
            {
                pushVarToDukStack(ctx, e);
                duk_put_prop_index(ctx, arr_idx, i++);
            }

            return;
        }
        if (v.isObject())
        {
            if (auto* o = v.getDynamicObject())
            {
                duk_idx_t obj_idx = duk_push_object(ctx);

                for (auto& e : o->getProperties())
                {
                    pushVarToDukStack(ctx, e.value);
                    duk_put_prop_string(ctx, obj_idx, e.name.toString().toRawUTF8());
                }
            }

            return;
        }
        if (v.isMethod())
        {
            // We wrap the native function to provide a helper layer storing and retrieving the
            // stash, and marshalling between the Duktape C interface and the NativeFunction interface
            duk_push_c_function(ctx, LambdaHelper::invokeFromDukContext, DUK_VARARGS);

            // Now we assign the pointers as properties of the wrapper function
            auto helper = std::make_unique<LambdaHelper>(v.getNativeFunction(), nextHelperId++);
            duk_push_pointer(ctx, (void *) helper.get());
            duk_put_prop_string(ctx, -2, DUK_HIDDEN_SYMBOL("LambdaHelperPtr"));
            duk_push_pointer(ctx, (void *) this);
            duk_put_prop_string(ctx, -2, DUK_HIDDEN_SYMBOL("EnginePtr"));

            // Now we prepare the finalizer
            duk_push_c_function(ctx, LambdaHelper::callbackFinalizer, 1);
            duk_push_pointer(ctx, (void *) helper.get());
            duk_put_prop_string(ctx, -2, DUK_HIDDEN_SYMBOL("LambdaHelperPtr"));
            duk_push_pointer(ctx, (void *) this);
            duk_put_prop_string(ctx, -2, DUK_HIDDEN_SYMBOL("EnginePtr"));
            duk_set_finalizer(ctx, -2);

            // And hang on to it!
            lambdaReleasePool[helper->id] = std::move(helper);
            return;
        }

        // If you hit this, you tried to push an unsupported var type to the duktape
        // stack.
        jassertfalse;
    }

    juce::var EcmascriptEngine::readVarFromDukStack (duk_context* ctx, duk_idx_t idx)
    {
        juce::var value;

        switch (duk_get_type(ctx, idx))
        {
            case DUK_TYPE_NULL:
                // It looks like juce::var doesn't have an explicit null value,
                // so we're just using the default empty constructor value.
                break;
            case DUK_TYPE_UNDEFINED:
                value = juce::var::undefined();
                break;
            case DUK_TYPE_BOOLEAN:
                value = (bool) duk_get_boolean(ctx, idx);
                break;
            case DUK_TYPE_NUMBER:
                value = duk_get_number(ctx, idx);
                break;
            case DUK_TYPE_STRING:
                value = juce::String(juce::CharPointer_UTF8(duk_get_string(ctx, idx)));
                break;
            case DUK_TYPE_OBJECT:
            {
                if (duk_is_array(ctx, idx))
                {
                    duk_size_t len = duk_get_length(ctx, idx);
                    juce::Array<juce::var> els;

                    for (duk_size_t i = 0; i < len; ++i)
                    {
                        duk_get_prop_index(ctx, idx, i);
                        els.add(readVarFromDukStack(ctx, -1));
                        duk_pop(ctx);
                    }

                    value = els;
                    break;
                }

                if (duk_is_function(ctx, idx))
                {
                    // With a function, we first push the function reference to
                    // the Duktape global stash so we can read it later.
                    auto funId = juce::String("__blueprintCallback__") + juce::Uuid().toString();

                    duk_push_global_stash(ctx);
                    duk_dup(ctx, idx);
                    duk_put_prop_string(ctx, -2, funId.toRawUTF8());

                    // Next we create a var::NativeFunction that captures the function
                    // id and knows how to invoke it
                    value = juce::var::NativeFunction {
                        [this, ctx, funId = std::move(funId)](const juce::var::NativeFunctionArgs& args) -> juce::var {
                            // Here when we're being invoked we retrieve the callback function from
                            // the global stash and invoke it with the provided args.
                            duk_push_global_stash(ctx);
                            duk_get_prop_string(ctx, -1, funId.toRawUTF8());
                            duk_require_function(ctx, -1);

                            // Push the args to the duktape stack
                            duk_require_stack_top(ctx, args.numArguments);

                            for (int i = 0; i < args.numArguments; ++i)
                                pushVarToDukStack(ctx, args.arguments[i]);

                            // Invocation
                            try {
                                detail::safeCall(ctx, args.numArguments);
                            } catch (Error const& err) {
                                reset();
                                throw err;
                            }

                            // Clean the result and the stash off the top of the stack
                            duk_pop_2(ctx);

                            // Here too, we know that we have potentially just pushed temporaries
                            // to the stack. Duktape will eventually get around to garbage collection,
                            // but specifically where we have LambdaHelper allocations for these
                            // temporaries, we want to keep our release pool fairly small. For that
                            // reason, we manually invoke a garbage collection pass right away.
                            // TODO: Revisit with lightfuncs
                            duk_gc(ctx, 0);

                            // Callbacks don't really need return args?
                            return juce::var();
                        }
                    };

                    break;
                }

                // If it's not a function or an array, it's a regular object.
                auto* obj = new juce::DynamicObject();

                // Generic object enumeration; `duk_enum` pushes an enumerator
                // object to the top of the stack
                duk_enum(ctx, idx, DUK_ENUM_OWN_PROPERTIES_ONLY);

                while (duk_next(ctx, -1, 1))
                {
                    // For each found key/value pair, `duk_enum` pushes the
                    // values to the top of the stack. So here the stack top
                    // is [ ... enum key value]. Enum is at -3, key at -2,
                    // value at -1 from the stack top.
                    // Note here that all keys in an ECMAScript object are of
                    // type string, even arrays, e.g. `myArr[0]` has an implicit
                    // conversion from number to string. Thus here, while constructing
                    // the DynamicObject, we take the `toString()` value for the key
                    // always.
                    obj->setProperty(duk_to_string(ctx, -2), readVarFromDukStack(ctx, -1));

                    // Clear the key/value pair from the stack
                    duk_pop_2(ctx);
                }

                // Pop the enumerator from the stack
                duk_pop(ctx);

                value = juce::var(obj);
                break;
            }
            case DUK_TYPE_NONE:
            default:
                jassertfalse;
        }

        return value;
    }

}
