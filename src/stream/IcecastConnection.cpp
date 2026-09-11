/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "stream/IcecastConnection.h"

namespace opendj
{

namespace
{
    constexpr int connectTimeoutMs = 5000;
    constexpr int responseTimeoutMs = 5000;
    constexpr int sendTimeoutMs = 2000;

    juce::String basicAuth (const juce::String& user, const juce::String& password)
    {
        return "Basic " + juce::Base64::toBase64 (user + ":" + password);
    }

    /** A header line, or nothing at all when the value is empty. An empty
        `Ice-Genre` is worse than no `Ice-Genre`: some servers publish it
        verbatim to the directory. */
    juce::String header (const juce::String& name, const juce::String& value)
    {
        return value.isEmpty() ? juce::String() : name + ": " + value + "\r\n";
    }
}

//==============================================================================

juce::String BroadcastSettings::normalisedMount() const
{
    const auto trimmed = mount.trim();

    if (trimmed.isEmpty())
        return "/";

    return trimmed.startsWithChar ('/') ? trimmed : "/" + trimmed;
}

juce::String BroadcastSettings::validate() const
{
    if (host.trim().isEmpty())
        return "The server address is empty.";

    if (! juce::isPositiveAndBelow (port, 65536) || port == 0)
        return "The port must be between 1 and 65535.";

    if (password.isEmpty())
        return "The server needs a password.";

    if (normalisedMount() == "/")
        return "The mount point is empty. Icecast usually wants something like /opendj.";

    return {};
}

//==============================================================================

juce::String buildSourceRequest (const BroadcastSettings& settings, bool useHttpPut)
{
    const auto mount = settings.normalisedMount();

    juce::String request;

    // The verb and version are the whole difference between the two forms. An
    // Icecast 2.4 server wants PUT and treats SOURCE as legacy; anything older,
    // and most of the Icecast-alikes, only know SOURCE.
    request << (useHttpPut ? "PUT " : "SOURCE ") << mount
            << (useHttpPut ? " HTTP/1.1\r\n" : " HTTP/1.0\r\n");

    request << "Host: " << settings.host << ":" << settings.port << "\r\n"
            << "Authorization: " << basicAuth (settings.user, settings.password) << "\r\n"
            << "User-Agent: OpenDJ\r\n"
            << "Content-Type: audio/ogg\r\n";

    request << header ("Ice-Name", settings.name)
            << header ("Ice-Description", settings.description)
            << header ("Ice-Genre", settings.genre)
            << header ("Ice-Url", settings.url)
            << "Ice-Public: " << (settings.isPublic ? "1" : "0") << "\r\n"
            << "Ice-Audio-Info: channels=2\r\n";

    // Asking to be told before sending a stream that will be refused. A server
    // that ignores this answers 200 instead, which is handled the same way.
    if (useHttpPut)
        request << "Expect: 100-continue\r\n";

    request << "\r\n";
    return request;
}

juce::String buildMetadataRequest (const BroadcastSettings& settings, const juce::String& title)
{
    const auto path = "/admin/metadata?mount=" + juce::URL::addEscapeChars (settings.normalisedMount(), false)
                    + "&mode=updinfo&song=" + juce::URL::addEscapeChars (title, true);

    juce::String request;

    request << "GET " << path << " HTTP/1.0\r\n"
            << "Host: " << settings.host << ":" << settings.port << "\r\n"
            << "Authorization: " << basicAuth (settings.user, settings.password) << "\r\n"
            << "User-Agent: OpenDJ\r\n"
            << "\r\n";

    return request;
}

//==============================================================================

IcecastConnection::IcecastConnection() = default;

IcecastConnection::~IcecastConnection() { disconnect(); }

bool IcecastConnection::connect (const BroadcastSettings& settings, juce::String& error)
{
    disconnect();

    if (error = settings.validate(); error.isNotEmpty())
        return false;

    // Both forms are tried against one connection each, PUT first. A server
    // that wants the other answers rather than hanging, so this costs a round
    // trip on old servers and nothing on new ones.
    for (const auto useHttpPut : { true, false })
    {
        socket = std::make_unique<juce::StreamingSocket>();

        if (! socket->connect (settings.host, settings.port, connectTimeoutMs))
        {
            error = "Could not reach " + settings.host + ":" + juce::String (settings.port) + ".";
            socket.reset();
            continue;
        }

        const auto request = buildSourceRequest (settings, useHttpPut);
        const auto bytes = request.toRawUTF8();
        const auto length = (int) strlen (bytes);

        if (socket->write (bytes, length) != length)
        {
            error = "The connection closed while sending the request.";
            socket.reset();
            continue;
        }

        if (readResponse (error))
        {
            connected.store (true, std::memory_order_relaxed);
            bytesSent.store (0, std::memory_order_relaxed);
            return true;
        }

        socket.reset();
    }

    return false;
}

bool IcecastConnection::readResponse (juce::String& error)
{
    juce::MemoryBlock response;
    char buffer[512];

    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) responseTimeoutMs;

    while (juce::Time::getMillisecondCounter() < deadline)
    {
        const auto ready = socket->waitUntilReady (true, 200);

        if (ready < 0)
            break;

        if (ready == 0)
            continue;

        const auto read = socket->read (buffer, (int) sizeof (buffer), false);

        if (read <= 0)
            break;

        response.append (buffer, (size_t) read);

        const juce::String text (juce::CharPointer_UTF8 ((const char*) response.getData()),
                                 response.getSize());

        if (! text.contains ("\r\n"))
            continue;

        const auto status = text.upToFirstOccurrenceOf ("\r\n", false, false);

        // 100 is the answer to Expect: 100-continue, 200 is a server that
        // ignored it and accepted anyway. Either means start sending.
        if (status.contains (" 200") || status.contains (" 100"))
            return true;

        if (status.contains (" 401") || status.contains (" 403"))
            error = "The server refused that user and password.";
        else if (status.contains (" 403"))
            error = "That mount point is already in use.";
        else if (status.isNotEmpty())
            error = "The server answered: " + status;
        else
            error = "The server said nothing useful.";

        return false;
    }

    if (error.isEmpty())
        error = "The server did not answer in time.";

    return false;
}

void IcecastConnection::disconnect()
{
    connected.store (false, std::memory_order_relaxed);

    if (socket != nullptr)
    {
        socket->close();
        socket.reset();
    }
}

bool IcecastConnection::send (const void* data, int numBytes)
{
    if (socket == nullptr || numBytes <= 0 || ! isConnected())
        return false;

    // Written in whatever pieces the socket will take, so one slow moment does
    // not throw away a page. A socket that has gone is reported rather than
    // retried: the broadcaster above knows how to reconnect and this does not.
    auto* bytes = static_cast<const char*> (data);
    auto remaining = numBytes;

    while (remaining > 0)
    {
        if (socket->waitUntilReady (false, sendTimeoutMs) <= 0)
        {
            disconnect();
            return false;
        }

        const auto written = socket->write (bytes, remaining);

        if (written <= 0)
        {
            disconnect();
            return false;
        }

        bytes += written;
        remaining -= written;
        bytesSent.fetch_add (written, std::memory_order_relaxed);
    }

    return true;
}

void IcecastConnection::sendMetadata (const BroadcastSettings& settings, const juce::String& title)
{
    if (title.isEmpty())
        return;

    juce::StreamingSocket admin;

    if (! admin.connect (settings.host, settings.port, connectTimeoutMs))
        return;

    const auto request = buildMetadataRequest (settings, title);
    const auto bytes = request.toRawUTF8();

    admin.write (bytes, (int) strlen (bytes));

    // The answer is not read. A title that failed to update is not worth
    // holding up anything, and the broadcast itself is on the other socket.
    admin.close();
}

} // namespace opendj
