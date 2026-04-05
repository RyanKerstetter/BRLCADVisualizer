#include "worker_ipc.h"

#include <vector>

namespace ibrt::ipc {

std::string makePipeName(uint32_t processId)
{
  return "\\\\.\\pipe\\IBRT.RenderWorker." + std::to_string(processId);
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
#endif

} // namespace ibrt::ipc
