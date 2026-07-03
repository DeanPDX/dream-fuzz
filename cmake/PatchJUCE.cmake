# Works around an MSVC name-lookup bug (error C2327 / C2065): nested lambdas
# inside a member function of a function-local class fail to resolve the
# class's own data members ('callbackPtr', 'dispatch') on some Visual Studio
# 2022 toolsets. Hoisting the members into locals before the lambdas is
# semantically identical and compiles everywhere.
#
# Applied at configure time, after FetchContent has populated JUCE.
# Idempotent: a marker comment prevents re-application. Uses single-line
# replacements so it is immune to CRLF/LF checkout differences.
#
# Affected file is unchanged upstream as of JUCE 8.0.14 and develop
# (checked 2026-07-03); remove this patch once JUCE fixes it.

set(_ump "${juce_SOURCE_DIR}/modules/juce_audio_basics/midi/ump/juce_UMPDispatcher.h")

if(EXISTS "${_ump}")
    file(READ "${_ump}" _content)

    if(NOT _content MATCHES "dreamfuzz-msvc-workaround")
        string(REPLACE
            "Conversion::toMidi1 ({ dispatch.group, msg.asSpan() }, [&] (const View& view)"
            "BytestreamToUMPDispatcher& dispatchRef = dispatch; auto* const cb = callbackPtr; // dreamfuzz-msvc-workaround (C2327)\n                Conversion::toMidi1 ({ dispatchRef.group, msg.asSpan() }, [&] (const View& view)"
            _content "${_content}")

        string(REPLACE
            "dispatch.converter.convert (view, [&] (const View& v)"
            "dispatchRef.converter.convert (view, [&] (const View& v)"
            _content "${_content}")

        string(REPLACE
            "(*callbackPtr) (v, msg.getTimeStamp());"
            "(*cb) (v, msg.getTimeStamp());"
            _content "${_content}")

        file(WRITE "${_ump}" "${_content}")
        message(STATUS "DreamFuzz: applied MSVC C2327 workaround to juce_UMPDispatcher.h")
    endif()
endif()

unset(_ump)
unset(_content)
