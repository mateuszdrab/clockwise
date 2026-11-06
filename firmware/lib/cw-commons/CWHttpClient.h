#pragma once

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FS.h>
#include <stdlib.h>
#include "SystemLog.h"

struct ClockwiseHttpClient
{
  static const uint32_t MAX_CANVAS_BYTES = 32768;

  static ClockwiseHttpClient *getInstance()
  {
    static ClockwiseHttpClient base;
    return &base;
  }

  bool getToFile(const String &url, File &output, uint8_t redirectCount = 0)
  {
    String requestUrl = url;
    const int fragment = requestUrl.indexOf('#');
    if (fragment >= 0)
      requestUrl.remove(fragment);
    String logUrl = requestUrl;
    const int query = logUrl.indexOf('?');
    if (query >= 0)
      logUrl.remove(query);
    SystemLog::instance().logf("[Canvas] Request: %s", logUrl.c_str());

    const int schemeEnd = requestUrl.indexOf("://");
    if (schemeEnd < 0) {
      SystemLog::instance().logf("[Canvas] Invalid URL: missing scheme");
      return false;
    }
    String scheme = requestUrl.substring(0, schemeEnd);
    scheme.toLowerCase();
    const bool secure = scheme == "https";
    if (!secure && scheme != "http") {
      SystemLog::instance().logf("[Canvas] Unsupported URL scheme: %s", scheme.c_str());
      return false;
    }

    const int authorityStart = schemeEnd + 3;
    int pathStart = requestUrl.indexOf('/', authorityStart);
    const int queryStart = requestUrl.indexOf('?', authorityStart);
    if (queryStart >= 0 && (pathStart < 0 || queryStart < pathStart))
      pathStart = queryStart;

    String authority = pathStart < 0 ? requestUrl.substring(authorityStart) : requestUrl.substring(authorityStart, pathStart);
    String path = pathStart < 0 ? "/" : requestUrl.substring(pathStart);
    if (path.startsWith("?"))
      path = "/" + path;

    uint16_t port = secure ? 443 : 80;
    const int portSeparator = authority.lastIndexOf(':');
    String host = authority;
    if (portSeparator > 0) {
      host = authority.substring(0, portSeparator);
      port = authority.substring(portSeparator + 1).toInt();
      if (port == 0)
        return false;
    }
    if (host.length() == 0 || host.indexOf('@') >= 0)
      return false;

    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    Client *client = &plainClient;
    if (secure) {
      secureClient.setInsecure();
      client = &secureClient;
    }
    client->setTimeout(10000);
    if (!client->connect(host.c_str(), port)) {
      SystemLog::instance().logf("[Canvas] Connect failed: %s", url.c_str());
      return false;
    }

    client->printf("GET %s HTTP/1.1\r\n", path.c_str());
    client->printf("Host: %s\r\n", authority.c_str());
    client->println("Accept: application/json");
    client->println("Connection: close");
    client->println();

    String status = client->readStringUntil('\n');
    status.trim();
    const int firstSpace = status.indexOf(' ');
    const int statusCode = firstSpace < 0 ? 0 : status.substring(firstSpace + 1, firstSpace + 4).toInt();
    if (statusCode != 200 && statusCode != 301 && statusCode != 302 && statusCode != 303 && statusCode != 307 && statusCode != 308) {
      SystemLog::instance().logf("[Canvas] Unexpected HTTP status: %s", status.c_str());
      client->stop();
      return false;
    }

    int32_t contentLength = -1;
    bool chunked = false;
    String redirectLocation;
    while (client->connected() || client->available()) {
      String header = client->readStringUntil('\n');
      header.trim();
      if (header.length() == 0)
        break;
      const int separator = header.indexOf(':');
      if (separator < 0)
        continue;
      String name = header.substring(0, separator);
      String value = header.substring(separator + 1);
      name.trim();
      value.trim();
      if (name.equalsIgnoreCase("Content-Length"))
        contentLength = value.toInt();
      else if (name.equalsIgnoreCase("Transfer-Encoding")) {
        value.toLowerCase();
        chunked = value.indexOf("chunked") >= 0;
      } else if (name.equalsIgnoreCase("Location"))
        redirectLocation = value;
    }

    if (statusCode != 200) {
      client->stop();
      if (redirectLocation.isEmpty()) {
        SystemLog::instance().logf("[Canvas] HTTP %d redirect from %s has no Location header", statusCode, logUrl.c_str());
        return false;
      }
      if (redirectCount >= 3) {
        SystemLog::instance().logf("[Canvas] Redirect limit reached (HTTP %d from %s)", statusCode, logUrl.c_str());
        return false;
      }

      String redirectedUrl = redirectLocation;
      if (redirectedUrl.startsWith("//"))
        redirectedUrl = scheme + ":" + redirectedUrl;
      else if (redirectedUrl.startsWith("/"))
        redirectedUrl = scheme + "://" + authority + redirectedUrl;
      else if (redirectedUrl.indexOf("://") < 0) {
        const int lastSlash = requestUrl.lastIndexOf('/');
        redirectedUrl = requestUrl.substring(0, lastSlash + 1) + redirectedUrl;
      }
      SystemLog::instance().logf("[Canvas] Following HTTP %d redirect to %s", statusCode, redirectedUrl.c_str());
      return getToFile(redirectedUrl, output, redirectCount + 1);
    }

    uint32_t written = 0;
    bool success;
    if (contentLength > static_cast<int32_t>(MAX_CANVAS_BYTES)) {
      SystemLog::instance().logf("[Canvas] Response too large: %ld bytes", static_cast<long>(contentLength));
      success = false;
    }
    else if (chunked)
      success = copyChunkedBody(*client, output, written);
    else if (contentLength >= 0)
      success = copyBytes(*client, output, static_cast<uint32_t>(contentLength), written);
    else
      success = copyUntilClose(*client, output, written);

    client->stop();
    SystemLog::instance().logf(success && written > 0 ? "[Canvas] Received %lu bytes" : "[Canvas] Response transfer failed after %lu bytes", static_cast<unsigned long>(written));
    return success && written > 0;
  }

  bool copyBytes(Client &client, File &output, uint32_t length, uint32_t &written)
  {
    uint8_t buffer[256];
    while (length > 0) {
      const size_t requested = length < sizeof(buffer) ? length : sizeof(buffer);
      const size_t received = client.readBytes(reinterpret_cast<char *>(buffer), requested);
      if (received == 0 || received > MAX_CANVAS_BYTES - written || output.write(buffer, received) != received)
        return false;
      length -= received;
      written += received;
    }
    return true;
  }

  bool copyChunkedBody(Client &client, File &output, uint32_t &written)
  {
    while (true) {
      String sizeLine = client.readStringUntil('\n');
      sizeLine.trim();
      const int extension = sizeLine.indexOf(';');
      if (extension >= 0)
        sizeLine.remove(extension);
      const uint32_t chunkSize = strtoul(sizeLine.c_str(), nullptr, 16);
      if (chunkSize == 0) {
        while (client.connected()) {
          String trailer = client.readStringUntil('\n');
          trailer.trim();
          if (trailer.length() == 0)
            break;
        }
        return true;
      }
      if (chunkSize > MAX_CANVAS_BYTES - written || !copyBytes(client, output, chunkSize, written))
        return false;
      client.readStringUntil('\n');
    }
  }

  bool copyUntilClose(Client &client, File &output, uint32_t &written)
  {
    uint8_t buffer[256];
    uint32_t lastDataMillis = millis();
    while (client.connected() || client.available()) {
      const int available = client.available();
      if (available <= 0) {
        if (millis() - lastDataMillis >= 10000)
          return false;
        delay(1);
        continue;
      }
      const size_t requested = available < static_cast<int>(sizeof(buffer)) ? available : sizeof(buffer);
      const size_t received = client.readBytes(reinterpret_cast<char *>(buffer), requested);
      if (received == 0 || received > MAX_CANVAS_BYTES - written || output.write(buffer, received) != received)
        return false;
      written += received;
      lastDataMillis = millis();
    }
    return true;
  }

  void httpGet(WiFiClientSecure *client, const char *host, const char *path, const uint16_t port)
  {
    SerialLog::instance().printf("[HTTP] GET request to '%s%s' on port %d\n", host, path, port);

    if (WiFi.status() != WL_CONNECTED)
    {
      SerialLog::instance().println("Not connected");
      return;
    }

    client->setInsecure();
    client->setTimeout(10000);
    if (!client->connect(host, port))
    {
      SerialLog::instance().println(F("Connection failed"));
      return;
    }

    // Send HTTP request
    client->printf("GET %s HTTP/1.1\r\n", path);
    client->printf("Host: %s\r\n", host);
    client->println(F("Connection: close"));
    if (client->println() == 0)
    {
      SerialLog::instance().println(F("Failed to send request"));
      client->stop();
      return;
    }

    // char arrCode[4];
    // memcpy(arrCode, status + 8, 3);  //HTTP/1.1 404 Not Found
    // arrCode[3] = 0;
    // uint16_t httpCode = atoi(arrCode);

    // Check HTTP status
    char status[32] = {0};
    client->readBytesUntil('\r', status, sizeof(status));

    if (strstr(status, "200 OK") == NULL)
    {
      SerialLog::instance().print(F("Unexpected response: "));
      SerialLog::instance().println(status);
      client->stop();
      return;
    }

    // Skip HTTP headers
    char endOfHeaders[] = "\r\n\r\n";
    if (!client->find(endOfHeaders))
    {
      SerialLog::instance().println(F("Invalid response"));
      client->stop();
      return;
    }
  }
};
