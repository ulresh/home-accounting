#include "QrCode.h"
#include <stdexcept>
#include <algorithm>
#include <cstdlib>

namespace qr {

static const int ECC_CODEWORDS_PER_BLOCK[4][41] = {
 {-1,7,10,15,20,26,18,20,24,30,18,20,24,26,30,22,24,28,30,28,28,28,28,30,30,26,28,30,30,30,30,30,30,30,30,30,30,30,30,30,30},
 {-1,10,16,26,18,24,16,18,22,22,26,30,22,22,24,24,28,28,26,26,26,26,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28},
 {-1,13,22,18,26,18,24,18,22,20,24,28,26,24,20,30,24,28,28,26,30,28,30,30,30,30,28,30,30,30,30,30,30,30,30,30,30,30,30,30,30},
 {-1,17,28,22,16,22,28,26,26,24,28,24,28,22,24,24,30,28,28,26,28,30,24,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30},
};
static const int NUM_ERROR_CORRECTION_BLOCKS[4][41] = {
 {-1,1,1,1,1,1,2,2,2,2,4,4,4,4,4,6,6,6,6,7,8,8,9,9,10,12,12,12,13,14,15,16,17,18,19,19,20,21,22,24,25},
 {-1,1,1,1,2,2,4,4,4,5,5,5,8,9,9,10,10,11,13,14,16,17,17,18,20,21,23,25,26,28,29,31,33,35,37,38,40,43,45,47,49},
 {-1,1,1,2,2,4,4,6,6,8,8,8,10,12,16,12,17,16,18,21,20,23,23,25,27,29,34,34,35,38,40,43,45,48,51,53,56,59,62,65,68},
 {-1,1,1,2,4,4,4,5,6,8,8,11,11,16,16,18,16,19,21,25,25,25,34,30,32,35,37,40,42,45,48,51,54,57,60,63,66,70,74,77,81},
};

// ---- битовый буфер ----
static void appendBits(std::vector<bool>& bb, unsigned int val, int len) {
    for (int i = len - 1; i >= 0; --i)
        bb.push_back(((val >> i) & 1) != 0);
}

int QrCode::getNumRawDataModules(int ver) {
    int result = (16 * ver + 128) * ver + 64;
    if (ver >= 2) {
        int numAlign = ver / 7 + 2;
        result -= (25 * numAlign - 10) * numAlign - 55;
        if (ver >= 7) result -= 36;
    }
    return result;
}

int QrCode::getNumDataCodewords(int ver, Ecc ecl) {
    int e = (int)ecl;
    return getNumRawDataModules(ver) / 8
         - ECC_CODEWORDS_PER_BLOCK[e][ver] * NUM_ERROR_CORRECTION_BLOCKS[e][ver];
}

uint8_t QrCode::reedSolomonMultiply(uint8_t x, uint8_t y) {
    int z = 0;
    for (int i = 7; i >= 0; --i) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return (uint8_t)z;
}

std::vector<uint8_t> QrCode::reedSolomonComputeDivisor(int degree) {
    std::vector<uint8_t> result(degree, 0);
    result[degree - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; ++i) {
        for (size_t j = 0; j < result.size(); ++j) {
            result[j] = reedSolomonMultiply(result[j], root);
            if (j + 1 < result.size()) result[j] ^= result[j + 1];
        }
        root = reedSolomonMultiply(root, 0x02);
    }
    return result;
}

std::vector<uint8_t> QrCode::reedSolomonComputeRemainder(const std::vector<uint8_t>& data,
                                                         const std::vector<uint8_t>& divisor) {
    std::vector<uint8_t> result(divisor.size(), 0);
    for (uint8_t b : data) {
        uint8_t factor = b ^ result[0];
        result.erase(result.begin());
        result.push_back(0);
        for (size_t j = 0; j < result.size(); ++j)
            result[j] ^= reedSolomonMultiply(divisor[j], factor);
    }
    return result;
}

QrCode QrCode::encode(const std::string& bytes, Ecc ecl) {
    // Подобрать минимальную версию.
    int dataLen = (int)bytes.size();
    int version = 0;
    for (int v = 1; v <= 40; ++v) {
        int ccbits = (v <= 9) ? 8 : 16;
        int dataBits = 4 + ccbits + 8 * dataLen;
        int capacity = getNumDataCodewords(v, ecl) * 8;
        if (dataBits <= capacity) { version = v; break; }
    }
    if (version == 0) throw std::length_error("data too long for QR");

    int ccbits = (version <= 9) ? 8 : 16;
    std::vector<bool> bb;
    appendBits(bb, 0x4, 4);                 // режим: байтовый
    appendBits(bb, (unsigned)dataLen, ccbits);
    for (unsigned char c : bytes) appendBits(bb, c, 8);

    int dataCapacityBits = getNumDataCodewords(version, ecl) * 8;
    int term = std::min(4, dataCapacityBits - (int)bb.size());
    appendBits(bb, 0, term);
    while (bb.size() % 8 != 0) bb.push_back(false);
    for (uint8_t pad = 0xEC; (int)bb.size() < dataCapacityBits; pad ^= (0xEC ^ 0x11))
        appendBits(bb, pad, 8);

    std::vector<uint8_t> dataCodewords(bb.size() / 8, 0);
    for (size_t i = 0; i < bb.size(); ++i)
        dataCodewords[i >> 3] |= (bb[i] ? 1 : 0) << (7 - (i & 7));

    // Выбор лучшей маски выполняется в конструкторе (mask = -1).
    return QrCode(version, ecl, dataCodewords, -1);
}

QrCode::QrCode(int version, Ecc ecl, const std::vector<uint8_t>& dataCodewords, int mask)
    : version_(version), size_(version * 4 + 17), ecl_(ecl) {
    modules_.assign(size_, std::vector<bool>(size_, false));
    isFunction_.assign(size_, std::vector<bool>(size_, false));

    drawFunctionPatterns();
    std::vector<uint8_t> allCodewords = addEccAndInterleave(dataCodewords);
    drawCodewords(allCodewords);

    if (mask < 0) {
        long minPenalty = -1;
        for (int i = 0; i < 8; ++i) {
            applyMask(i);
            drawFormatBits(i);
            long penalty = getPenaltyScore();
            if (minPenalty < 0 || penalty < minPenalty) { mask = i; minPenalty = penalty; }
            applyMask(i); // отменить
        }
    }
    applyMask(mask);
    drawFormatBits(mask);
}

void QrCode::setFunctionModule(int x, int y, bool isDark) {
    modules_[y][x] = isDark;
    isFunction_[y][x] = true;
}

bool QrCode::module(int x, int y) const {
    if (x < 0 || x >= size_ || y < 0 || y >= size_) return false;
    return modules_[y][x];
}

void QrCode::drawFinderPattern(int x, int y) {
    for (int dy = -4; dy <= 4; ++dy) {
        for (int dx = -4; dx <= 4; ++dx) {
            int dist = std::max(std::abs(dx), std::abs(dy));
            int xx = x + dx, yy = y + dy;
            if (xx >= 0 && xx < size_ && yy >= 0 && yy < size_)
                setFunctionModule(xx, yy, dist != 2 && dist != 4);
        }
    }
}

void QrCode::drawAlignmentPattern(int x, int y) {
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx)
            setFunctionModule(x + dx, y + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
}

std::vector<int> QrCode::getAlignmentPatternPositions() const {
    if (version_ == 1) return {};
    int numAlign = version_ / 7 + 2;
    int step = (version_ == 32) ? 26
             : (version_ * 4 + numAlign * 2 + 1) / (numAlign * 2 - 2) * 2;
    std::vector<int> result;
    for (int i = 0, pos = size_ - 7; i < numAlign - 1; ++i, pos -= step)
        result.insert(result.begin(), pos);
    result.insert(result.begin(), 6);
    return result;
}

void QrCode::drawFunctionPatterns() {
    // тайминг
    for (int i = 0; i < size_; ++i) {
        setFunctionModule(6, i, i % 2 == 0);
        setFunctionModule(i, 6, i % 2 == 0);
    }
    // три искателя
    drawFinderPattern(3, 3);
    drawFinderPattern(size_ - 4, 3);
    drawFinderPattern(3, size_ - 4);

    // выравнивающие
    std::vector<int> align = getAlignmentPatternPositions();
    int n = (int)align.size();
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            if ((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0))
                continue;
            drawAlignmentPattern(align[i], align[j]);
        }

    drawFormatBits(0);
    drawVersion();
}

void QrCode::drawFormatBits(int mask) {
    int data = ((int)ecl_ << 3) | mask;  // здесь ecl уже в формате 01/00/11/10? нормализуем ниже
    // нормализация ecl к битам формата
    static const int FMT[4] = {1, 0, 3, 2}; // L=01,M=00,Q=11,H=10
    data = (FMT[(int)ecl_] << 3) | mask;
    int rem = data;
    for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int bits = ((data << 10) | rem) ^ 0x5412;

    for (int i = 0; i <= 5; ++i) setFunctionModule(8, i, ((bits >> i) & 1) != 0);
    setFunctionModule(8, 7, ((bits >> 6) & 1) != 0);
    setFunctionModule(8, 8, ((bits >> 7) & 1) != 0);
    setFunctionModule(7, 8, ((bits >> 8) & 1) != 0);
    for (int i = 9; i < 15; ++i) setFunctionModule(14 - i, 8, ((bits >> i) & 1) != 0);

    for (int i = 0; i < 8; ++i) setFunctionModule(size_ - 1 - i, 8, ((bits >> i) & 1) != 0);
    for (int i = 8; i < 15; ++i) setFunctionModule(8, size_ - 15 + i, ((bits >> i) & 1) != 0);
    setFunctionModule(8, size_ - 8, true); // тёмный модуль
}

void QrCode::drawVersion() {
    if (version_ < 7) return;
    int rem = version_;
    for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    long bits = ((long)version_ << 12) | rem;
    for (int i = 0; i < 18; ++i) {
        bool bit = ((bits >> i) & 1) != 0;
        int a = size_ - 11 + i % 3, b = i / 3;
        setFunctionModule(a, b, bit);
        setFunctionModule(b, a, bit);
    }
}

std::vector<uint8_t> QrCode::addEccAndInterleave(const std::vector<uint8_t>& data) const {
    int e = (int)ecl_;
    int numBlocks = NUM_ERROR_CORRECTION_BLOCKS[e][version_];
    int blockEccLen = ECC_CODEWORDS_PER_BLOCK[e][version_];
    int rawCodewords = getNumRawDataModules(version_) / 8;
    int numShortBlocks = numBlocks - rawCodewords % numBlocks;
    int shortBlockLen = rawCodewords / numBlocks;

    std::vector<std::vector<uint8_t>> blocks;
    std::vector<uint8_t> rsDiv = reedSolomonComputeDivisor(blockEccLen);
    for (int i = 0, k = 0; i < numBlocks; ++i) {
        int datLen = shortBlockLen - blockEccLen + (i < numShortBlocks ? 0 : 1);
        std::vector<uint8_t> dat(data.begin() + k, data.begin() + k + datLen);
        k += datLen;
        std::vector<uint8_t> ecc = reedSolomonComputeRemainder(dat, rsDiv);
        if (i < numShortBlocks) dat.push_back(0);
        dat.insert(dat.end(), ecc.begin(), ecc.end());
        blocks.push_back(std::move(dat));
    }

    std::vector<uint8_t> result;
    for (size_t i = 0; i < blocks[0].size(); ++i) {
        for (size_t j = 0; j < blocks.size(); ++j) {
            if (i != (size_t)(shortBlockLen - blockEccLen) || j >= (size_t)numShortBlocks)
                result.push_back(blocks[j][i]);
        }
    }
    return result;
}

void QrCode::drawCodewords(const std::vector<uint8_t>& data) {
    size_t i = 0;
    for (int right = size_ - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < size_; ++vert) {
            for (int j = 0; j < 2; ++j) {
                int x = right - j;
                bool upward = ((right + 1) & 2) == 0;
                int y = upward ? size_ - 1 - vert : vert;
                if (!isFunction_[y][x] && i < data.size() * 8) {
                    modules_[y][x] = ((data[i >> 3] >> (7 - (i & 7))) & 1) != 0;
                    ++i;
                }
            }
        }
    }
}

void QrCode::applyMask(int mask) {
    for (int y = 0; y < size_; ++y) {
        for (int x = 0; x < size_; ++x) {
            if (isFunction_[y][x]) continue;
            bool invert = false;
            switch (mask) {
                case 0: invert = (x + y) % 2 == 0; break;
                case 1: invert = y % 2 == 0; break;
                case 2: invert = x % 3 == 0; break;
                case 3: invert = (x + y) % 3 == 0; break;
                case 4: invert = (x / 3 + y / 2) % 2 == 0; break;
                case 5: invert = x * y % 2 + x * y % 3 == 0; break;
                case 6: invert = (x * y % 2 + x * y % 3) % 2 == 0; break;
                case 7: invert = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
            }
            modules_[y][x] = modules_[y][x] ^ invert;
        }
    }
}

long QrCode::getPenaltyScore() const {
    long result = 0;
    const int S = size_;
    // строки/столбцы — серии и шаблоны finder-подобные
    for (int y = 0; y < S; ++y) {
        bool runColor = false; int runX = 0;
        int runHistory[7] = {0};
        for (int x = 0; x < S; ++x) {
            if (modules_[y][x] == runColor) {
                ++runX;
                if (runX == 5) result += 3;
                else if (runX > 5) ++result;
            } else { runColor = modules_[y][x]; runX = 1; }
            (void)runHistory;
        }
    }
    for (int x = 0; x < S; ++x) {
        bool runColor = false; int runY = 0;
        for (int y = 0; y < S; ++y) {
            if (modules_[y][x] == runColor) {
                ++runY;
                if (runY == 5) result += 3;
                else if (runY > 5) ++result;
            } else { runColor = modules_[y][x]; runY = 1; }
        }
    }
    // блоки 2x2
    for (int y = 0; y < S - 1; ++y)
        for (int x = 0; x < S - 1; ++x) {
            bool c = modules_[y][x];
            if (c == modules_[y][x+1] && c == modules_[y+1][x] && c == modules_[y+1][x+1])
                result += 3;
        }
    // баланс тёмных/светлых
    int dark = 0;
    for (int y = 0; y < S; ++y) for (int x = 0; x < S; ++x) if (modules_[y][x]) ++dark;
    int total = S * S;
    int k = 0;
    while (std::abs(dark * 20 - total * 10) > (k + 1) * total) ++k;
    result += k * 10;
    return result;
}

} // namespace qr
