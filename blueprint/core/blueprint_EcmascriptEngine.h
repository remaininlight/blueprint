/*
  ==============================================================================

    blueprint_EcmascriptEngine.h
    Created: 24 Oct 2019 3:08:39pm

  ==============================================================================
*/

#pragma once

#include <unordered_map>


namespace blueprint
{

    //==============================================================================
    /** The EcmascriptEngine provides a flexible ECMAScript 5 compliant JavaScript engine
     *  with an interface implemented by Duktape, but which may be implemented by one of
     *  many embedded engines in the future.
     */
    class EcmascriptEngine : private juce::Timer
    {
    public:
        //==============================================================================
        EcmascriptEngine();
        ~EcmascriptEngine();

        //==============================================================================
        /** A helper struct for representing an error that occured within the Duktape
         *  engine.
         *
         *  We provide the JavaScript stack trace in the `stack` member.
         */
        struct Error : public std::runtime_error {
            Error(const juce::String& msg)
                : std::runtime_error(msg.toStdString()) {}

            Error(const juce::String& msg, const juce::String& _stack)
                : std::runtime_error(msg.toStdString()), stack(_stack) {}

            Error(const juce::String& msg, const juce::String& _stack, const juce::String& _context)
                : std::runtime_error(msg.toStdString()), stack(_stack), context(_context) {}

            const char* what() const throw()
            {
                auto base = juce::String(std::runtime_error::what());
                auto full = base + "\n" + context;
                return full.toRawUTF8();
            }

            juce::String stack;
            juce::String context;
        };

        /** A helper struct for representing an error that occured within the Duktape
         *  engine.
         *
         *  In the event this error is thrown, the engine is to be considered
         *  unrecoverable, and it is up to the user to address how to proceed.
         */
        struct FatalError : public std::runtime_error {
            FatalError(const juce::String& msg)
                : std::runtime_error(msg.toStdString()) {}
        };

        //==============================================================================
        /** Evaluates the given code in the interpreter, returning the result.
         *
         *  @returns juce::var result of the evaluation
         *  @throws EcmascriptEngine::Error in the event of an evaluation error
         */
        juce::var evaluate (const juce::String& code);
        juce::var evaluate (const juce::File& code);

        //==============================================================================
        /** Registers a native method by the given name in the global namespace. */
        void registerNativeMethod (const juce::String&, juce::var::NativeFunction fn);

        /** Registers a native method by the given name on the target object.
         *
         *  The provided target name may be any expression that leaves the target
         *  object on the top of the stack. For example:
         *
         *  ```
         *  registerNativeMethod("global", "hello", []() {
         *      std::cout << "World!" << std::endl;
         *      return juce::var();
         *  });
         *  ```
         *
         *  is equivalent to calling the previous `registerNativeMethod` overload
         *  with just the "hello" and function arguments.
         *
         *  @throws EcmascriptEngine::Error in the event of an evaluation error
         */
        void registerNativeMethod (const juce::String&, const juce::String&, juce::var::NativeFunction fn);

        //==============================================================================
        /** Registers a native value by the given name in the global namespace. */
        void registerNativeProperty (const juce::String&, const juce::var&);

        /** Registers a native value by the given name on the target object.
         *
         *  The provided target name may be any expression that leaves the target
         *  object on the top of the stack. For example, the following three
         *  examples have equivalent behavior:
         *
         *  ```
         *  registerNativeProperty("global", "hello", "world");
         *  registerNativeProperty("hello", "world");
         *  evaluate("global.hello = \"world\";");
         *  ```
         *
         *  @throws EcmascriptEngine::Error in the event of an evaluation error
         */
        void registerNativeProperty (const juce::String&, const juce::String&, const juce::var&);

        //==============================================================================
        /** Invokes a method, applying the given args, inside the interpreter.
         *
         *  This is similar in function to `Function.prototype.apply()`. The provided
         *  method name may be any expression that leaves the target function on the
         *  top of the stack. For example:
         *
         *  `invoke("BlueprintNative.dispatchViewEvent", args);`
         *
         *  @returns juce::var result of the invocation
         *  @throws EcmascriptEngine::Error in the event of an error
         */
        juce::var invoke (const juce::String& name, const std::vector<juce::var>& vargs);

        /** Invokes a method with the given args inside the interpreter.
         *
         *  This is similar in function to `Function.prototype.call()`. The provided
         *  method name may be any expression that leaves the target function on the
         *  top of the stack. For example:
         *
         *  `invoke("BlueprintNative.dispatchViewEvent", "click");`
         *
         *  @returns juce::var result of the invocation
         *  @throws EcmascriptEngine::Error in the event of an error
         */
        template <typename... T>
        juce::var invoke (const juce::String& name, T... args);

        //==============================================================================
        /** Resets the internal Duktape context, clearing the value stack and destroying native callbacks. */
        void reset();

        //==============================================================================
        /** Pauses execution and waits for a debug client to attach and begin a debug session. */
        void debuggerAttach();

        /** Detaches the from the current debug session/attachment. */
        void debuggerDetach();

    private:
        //==============================================================================
        void timerCallback() override;

        //==============================================================================
        struct LambdaHelper {
            LambdaHelper(juce::var::NativeFunction fn, uint32_t _id);

            static duk_ret_t invokeFromDukContext(duk_context* ctx);
            static duk_ret_t invokeFromDukContextLightFunc(duk_context* ctx);
            static duk_ret_t callbackFinalizer (duk_context* ctx);

            juce::var::NativeFunction callback;
            uint32_t id;
        };

        //==============================================================================
        duk_context* ctx;
        uint32_t nextHelperId = 0;
        int32_t nextMagicInt = 0;
        std::unordered_map<uint32_t, std::unique_ptr<LambdaHelper>> persistentReleasePool;
        std::unordered_map<int16_t, std::unique_ptr<LambdaHelper>> temporaryReleasePool;

        //==============================================================================
        /** Helper for cleaning up native function temporaries. */
        void removeLambdaHelper (LambdaHelper* helper);

        /** Helper for pushing a juce::var to the duktape stack. */
        void pushVarToDukStack (duk_context* ctx, const juce::var& v, bool persistNativeFunctions = false);

        /** Helper for reading from the duktape stack to a juce::var instance. */
        juce::var readVarFromDukStack (duk_context* ctx, duk_idx_t idx);

        //==============================================================================
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EcmascriptEngine)
    };

    //==============================================================================
    template <typename... T>
    juce::var EcmascriptEngine::invoke (const juce::String& name, T... args)
    {
        // Pack the args and push them to the alternate `invoke` implementation
        std::vector<juce::var> vargs { args... };
        return invoke(name, vargs);
    }

}
