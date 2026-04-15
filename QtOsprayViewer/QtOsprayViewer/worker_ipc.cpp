#include "worker_ipc.h"

#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace ibrt::ipc {

std::string makePipeName(uint32_t processId)
{
#ifdef _WIN32
  return "\\\\.\\pipe\\IBRT.RenderWorker." + std::to_string(processId);
#else
  return "/tmp/ibrt_render_" + std::to_string(processId) + ".sock";
#endif
}

#ifdef _WIN32
namespace {

bool writeAll(HANDLE pipe, const void *data, DWORD size)
{
  const auto *bytes = static_cast<const uint8_t *>(data);
  DWORD totalWritten = 0;
  while (totalWritten < size) {
    DWORD written = 0;
    if (!WriteFile(pipe, bytes + totalWritten, size - totalWritten, &written, nullptr))
      return false;
    totalWritten += written;
  }
  return true;
}

bool readAll(HANDLE pipe, void *data, DWORD size)
{
  auto *bytes = static_cast<uint8_t *>(data);
  DWORD totalRead = 0;
  while (totalRead < size) {
    DWORD read = 0;
    if (!ReadFile(pipe, bytes + totalRead, size - totalRead, &read, nullptr))
      return false;
    if (read == 0)
      return false;
    totalRead += read;
  }
  return true;
}

} // namespace

bool writeMessage(HANDLE pipe, const Message &message)
{
  MessageHeader header;
  header.type = static_cast<uint32_t>(message.type);
  header.requestId = message.requestId;
  header.payloadSize = static_cast<uint32_t>(message.payload.size());

  if (!writeAll(pipe, &header, sizeof(header)))
    return false;

  if (!message.payload.empty()
      && !writeAll(pipe, message.payload.data(), static_cast<DWORD>(message.payload.size()))) {
    return false;
  }

  return true;
}

bool readMessage(HANDLE pipe, Message &message)
{
  MessageHeader header;
  if (!readAll(pipe, &header, sizeof(header)))
    return false;

  if (header.magic != 0x54425249 || header.version != 1)
    return false;

  message.type = static_cast<MessageType>(header.type);
  message.requestId = header.requestId;
  message.payload.clear();

  if (header.payloadSize == 0)
    return true;

  std::vector<char> buffer(header.payloadSize);
  if (!readAll(pipe, buffer.data(), header.payloadSize))
    return false;

  message.payload.assign(buffer.begin(), buffer.end());
  return true;
}
#else
namespace {

bool writeAll(int fd, const void *data, size_t size)
{
  const auto *bytes = static_cast<const uint8_t *>(data);
  size_t totalWritten = 0;
  while (totalWritten < size) {
    ssize_t written = ::write(fd, bytes + totalWritten, size - totalWritten);
    if (written <= 0)
      return false;
    totalWritten += static_cast<size_t>(written);
  }
  return true;
}

bool readAll(int fd, void *data, size_t size)
{
  auto *bytes = static_cast<uint8_t *>(data);
  size_t totalRead = 0;
  while (totalRead < size) {
    ssize_t n = ::read(fd, bytes + totalRead, size - totalRead);
    if (n <= 0)
      return false;
    totalRead += static_cast<size_t>(n);
  }
  return true;
}

} // namespace

bool writeMessage(int fd, const Message &message)
{
  MessageHeader header;
  header.type = static_cast<uint32_t>(message.type);
  header.requestId = message.requestId;
  header.payloadSize = static_cast<uint32_t>(message.payload.size());

  if (!writeAll(fd, &header, sizeof(header)))
    return false;

  if (!message.payload.empty()
      && !writeAll(fd, message.payload.data(), message.payload.size())) {
    return false;
  }
  return true;
}

bool readMessage(int fd, Message &message)
{
  MessageHeader header;
  if (!readAll(fd, &header, sizeof(header)))
    return false;

  if (header.magic != 0x54425249 || header.version != 1)
    return false;

  message.type = static_cast<MessageType>(header.type);
  message.requestId = header.requestId;
  message.payload.clear();

  if (header.payloadSize == 0)
    return true;

  std::vector<char> buffer(header.payloadSize);
  if (!readAll(fd, buffer.data(), header.payloadSize))
    return false;

  message.payload.assign(buffer.begin(), buffer.end());
  return true;
}
#endif

} // namespace ibrt::ipc
