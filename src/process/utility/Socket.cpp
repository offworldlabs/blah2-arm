#include "Socket.h"
#include <iostream>

asio::io_context Socket::io_context;
const uint32_t Socket::MTU = 1024;

Socket::Socket(const std::string& ip, uint16_t port)
    : endpoint(asio::ip::address::from_string(ip), port), socket(io_context) {
    try {
        socket.connect(endpoint);
    } catch (const std::exception& e) {
        std::cerr << "Error connecting to endpoint: " << e.what() << std::endl;
        throw;
    }
}

Socket::~Socket()
{
}

void Socket::sendData(const std::string& data) {
    // ASIO's composed write retries short writes until the whole frame has
    // been sent. It borrows this string rather than allocating 1KiB substrings.
    // Failure propagates to the caller: continuing would corrupt JSON framing.
    asio::write(socket, asio::buffer(data.data(), data.size()));
}
