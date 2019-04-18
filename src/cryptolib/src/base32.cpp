
#include <array>
#include <vector>
#include <cassert>
#include <QByteArray>


namespace ds {
namespace crypto {

using namespace std;

QByteArray onion16decode(const QByteArray& src) {
    //Refactored from eschalot.c
    array<uint8_t, 16> tmp = {};

    if (src.size() != 16) {
        throw runtime_error("onion16decode: source size must be 16");
    }

    for (int i = 0; i < src.size(); i++) {
        if (static_cast<uint8_t>(src.at(i)) >= 'a'
                && static_cast<uint8_t>(src.at(i)) <= 'z') {
            tmp.at(static_cast<size_t>(i)) = static_cast<unsigned char>((src.at(i)) - ('a'));
        } else {
            if (static_cast<uint8_t>(src.at(i)) >= '2'
                    && static_cast<uint8_t>(src.at(i)) <= '7')
                tmp.at(static_cast<size_t>(i)) = static_cast<unsigned char>(src.at(i)) - '1' + ('z' - 'a');
            else {
                /* Bad character detected.
                 * This should not happen, but just in case
                 * we will replace it with 'z' character. */
                tmp.at(static_cast<size_t>(i)) = 26;
            }
        }
    }
    std::vector<uint8_t> dst;
    //QByteArray dst;
    dst.push_back(static_cast<uint8_t>(tmp.at( 0) << 3) | static_cast<uint8_t>(tmp.at(1) >> 2));
    dst.push_back(static_cast<uint8_t>(tmp.at( 1) << 6) | static_cast<uint8_t>(tmp.at(2) << 1) | static_cast<uint8_t>(tmp.at(3) >> 4));
    dst.push_back(static_cast<uint8_t>(tmp.at( 3) << 4) | static_cast<uint8_t>(tmp.at(4) >> 1));
    dst.push_back(static_cast<uint8_t>(tmp.at( 4) << 7) | static_cast<uint8_t>(tmp.at(5) << 2) | (tmp.at(6) >> 3));
    dst.push_back(static_cast<uint8_t>(tmp.at( 6) << 5) | static_cast<uint8_t>(tmp.at(7)));
    dst.push_back(static_cast<uint8_t>(tmp.at( 8) << 3) | static_cast<uint8_t>(tmp.at(9) >> 2));
    dst.push_back(static_cast<uint8_t>(tmp.at( 9) << 6) | static_cast<uint8_t>(tmp.at(10) << 1) | (tmp.at(11) >> 4));
    dst.push_back(static_cast<uint8_t>(tmp.at(11) << 4) | static_cast<uint8_t>(tmp.at(12) >> 1));
    dst.push_back(static_cast<uint8_t>(tmp.at(12) << 7) | static_cast<uint8_t>(tmp.at(13) << 2) | (tmp.at(14) >> 3));
    dst.push_back(static_cast<uint8_t>(tmp.at(14) << 5) | static_cast<uint8_t>(tmp.at(15)));

    return QByteArray{reinterpret_cast<const char *>(dst.data()), static_cast<int>(dst.size())};
}

/* Decode base32 16 character long 'src' into 10 byte long 'dst'. */
/* TODO: Revisit and review, would like to shrink it down a bit.
 * However, it has to stay endian-safe and be fast. */


QByteArray onion16encode(const QByteArray& src) {
    //Refactored from eschalot.c
    static const array<char, 32> alphabet = {
        'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i',
        'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r',
        's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '2',
        '3', '4', '5', '6', '7'
    };

    if (src.size() != 10) {
        throw runtime_error("onion16encode: source size must be 10");
    }

    QByteArray dst;
    dst.resize(16);

    auto usrc = reinterpret_cast<const uint8_t *>(src.constData());

    dst[ 0] = alphabet.at( (usrc[0] >> 3)			    );
    dst[ 1] = alphabet.at(((usrc[0] << 2) | (usrc[1] >> 6))	& 31);
    dst[ 2] = alphabet.at( (usrc[1] >> 1) 			& 31);
    dst[ 3] = alphabet.at(((usrc[1] << 4) | (usrc[2] >> 4))	& 31);
    dst[ 4] = alphabet.at(((usrc[2] << 1) | (usrc[3] >> 7))	& 31);
    dst[ 5] = alphabet.at( (usrc[3] >> 2)			& 31);
    dst[ 6] = alphabet.at(((usrc[3] << 3) | (usrc[4] >> 5))	& 31);
    dst[ 7] = alphabet.at(  usrc[4]				& 31);

    dst[ 8] = alphabet.at( (usrc[5] >> 3)			    );
    dst[ 9] = alphabet.at(((usrc[5] << 2) | (usrc[6] >> 6))	& 31);
    dst[10] = alphabet.at( (usrc[6] >> 1)			& 31);
    dst[11] = alphabet.at(((usrc[6] << 4) | (usrc[7] >> 4))	& 31);
    dst[12] = alphabet.at(((usrc[7] << 1) | (usrc[8] >> 7))	& 31);
    dst[13] = alphabet.at( (usrc[8] >> 2)			& 31);
    dst[14] = alphabet.at(((usrc[8] << 3) | (usrc[9] >> 5))	& 31);
    dst[15] = alphabet.at(  usrc[9]				& 31);

    return dst;
}


}} //namespaces
