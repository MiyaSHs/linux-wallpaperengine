#include <bit>
#include <iostream>

#include "BinaryReader.h"

#include <cstring>

using namespace WallpaperEngine::Data::Utils;

BinaryReader::BinaryReader (ReadStreamSharedPtr file) : m_input (std::move (file)) { }

uint32_t BinaryReader::nextUInt32 () const {
    char buffer[4];

    this->m_input->read (buffer, 4);

    if constexpr (std::endian::native == std::endian::little) {
	return (buffer[3] & 0xFF) << 24 | (buffer[2] & 0xFF) << 16 | (buffer[1] & 0xFF) << 8 | (buffer[0] & 0xFF);
    } else {
	return (buffer[0] & 0xFF) << 24 | (buffer[1] & 0xFF) << 16 | (buffer[2] & 0xFF) << 8 | (buffer[3] & 0xFF);
    }
}

int BinaryReader::nextInt () const {
    char buffer[4];

    this->m_input->read (buffer, 4);

    if constexpr (std::endian::native == std::endian::little) {
	return (buffer[3] & 0xFF) << 24 | (buffer[2] & 0xFF) << 16 | (buffer[1] & 0xFF) << 8 | (buffer[0] & 0xFF);
    } else {
	return (buffer[0] & 0xFF) << 24 | (buffer[1] & 0xFF) << 16 | (buffer[2] & 0xFF) << 8 | (buffer[3] & 0xFF);
    }
}

int32_t BinaryReader::nextInt32 () const {
    static_assert (std::endian::native == std::endian::little, "Little-endian only");
    int32_t result;
    this->m_input->read (reinterpret_cast<char*> (&result), sizeof (result));
    return result;
}

uint16_t BinaryReader::nextUInt16 () const {
    static_assert (std::endian::native == std::endian::little, "Little-endian only");
    uint16_t result;
    this->m_input->read (reinterpret_cast<char*> (&result), sizeof (result));
    return result;
}

int16_t BinaryReader::nextInt16 () const {
    static_assert (std::endian::native == std::endian::little, "Little-endian only");
    int16_t result;
    this->m_input->read (reinterpret_cast<char*> (&result), sizeof (result));
    return result;
}

uint8_t BinaryReader::nextUInt8 () const {
    uint8_t result;
    this->m_input->read (reinterpret_cast<char*> (&result), sizeof (result));
    return result;
}

float BinaryReader::nextFloat () const {
    float result;
    static_assert (std::endian::native == std::endian::little, "Only little endian is supported for floats");

    this->m_input->read (reinterpret_cast<char*> (&result), sizeof (result));

    return result;
}

std::string BinaryReader::nextNullTerminatedString () const {
    std::string output;

    while (const auto c = this->next ()) {
	output += c;
    }

    return output;
}

std::string BinaryReader::nextSizedString () const {
    uint32_t length = this->nextUInt32 ();
    std::string output (length, '\0');

    this->m_input->read (output.data (), length);

    return output;
}

void BinaryReader::next (char* out, size_t size) const { this->m_input->read (out, size); }

char BinaryReader::next () const {
    char buffer;
    this->m_input->read (&buffer, 1);
    return buffer;
}

std::istream& BinaryReader::base () const { return *this->m_input; }
