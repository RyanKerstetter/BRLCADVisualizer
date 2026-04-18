#include "worker_ipc.h"

#include <vector>

namespace ibrt::ipc {

// Builds a unique pipe name for a viewer/worker pair based on the viewer process ID.
std::string makePipeName(uint32_t processId)
{
  return "\\\\.\\pipe\\IBRT.RenderWorker." + std::to_string(processId);
}

#ifdef _WIN32
namespace {

// Writes a full message buffer to the pipe, retrying until all bytes are sent.
bool writeAll(HANDLE pipe, const void *data, DWORD size)
{
  // Named-pipe writes are not guaranteed to complete in one call.
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

// Reads an exact byte count from the pipe before returning control to the caller.
bool readAll(HANDLE pipe, void *data, DWORD size)
{
  // Mirror writeAll() so higher-level message parsing can assume exact sizes.
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

// Serializes and writes one protocol message to the named pipe.
bool writeMessage(HANDLE pipe, const Message &message)
{
  // Every payload is preceded by a fixed-size header so the receiver can safely
  // validate message type and byte count before parsing.
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

// Reads and validates one protocol message from the named pipe.
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
#endif

} // namespace ibrt::ipc
