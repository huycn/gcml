// Port of TestInfrastructure/Objects/TestStandardStreams.cs
#pragma once

#include "core/streams.h"

namespace gcm::test {

class TestStandardStreams : public IStandardStreams
{
public:
    /// Text served by in(). The reader is created on first use and recreated when `In` changes.
    QString In;
    StringWriter Out;
    StringWriter Error;
    bool IsInputRedirected = true;
    bool IsOutputRedirected = true;
    bool IsErrorRedirected = true;

    QString outText() const { return Out.toString(); }
    QString errorText() const { return Error.toString(); }

    // IStandardStreams
    TextReader &in() override
    {
        if (!m_reader || m_readerText != In) {
            m_readerText = In;
            m_reader = std::make_unique<StringReader>(In);
        }
        return *m_reader;
    }
    TextWriter &out() override { return Out; }
    TextWriter &error() override { return Error; }
    bool isInputRedirected() const override { return IsInputRedirected; }
    bool isOutputRedirected() const override { return IsOutputRedirected; }
    bool isErrorRedirected() const override { return IsErrorRedirected; }

private:
    QString m_readerText;
    std::unique_ptr<StringReader> m_reader;
};

} // namespace gcm::test
