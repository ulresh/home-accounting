#pragma once
#include <vector>
#include <string>
#include <cstdint>

// Компактный самодостаточный генератор QR Code (byte mode, ECC L/M/Q/H).
// Алгоритм по общедоступной справочной реализации Project Nayuki.
namespace qr {

enum class Ecc { LOW = 0, MEDIUM = 1, QUARTILE = 2, HIGH = 3 };

class QrCode {
public:
    // Закодировать произвольные байты (UTF-8 строку) в QR.
    static QrCode encode(const std::string& bytes, Ecc ecl);

    int size() const { return size_; }
    bool module(int x, int y) const; // true = тёмный

private:
    QrCode(int version, Ecc ecl, const std::vector<uint8_t>& dataCodewords, int mask);

    int version_;
    int size_;
    Ecc ecl_;
    std::vector<std::vector<bool>> modules_;
    std::vector<std::vector<bool>> isFunction_;

    void drawFunctionPatterns();
    void drawFormatBits(int mask);
    void drawVersion();
    void drawFinderPattern(int x, int y);
    void drawAlignmentPattern(int x, int y);
    void setFunctionModule(int x, int y, bool isDark);
    std::vector<uint8_t> addEccAndInterleave(const std::vector<uint8_t>& data) const;
    void drawCodewords(const std::vector<uint8_t>& data);
    void applyMask(int mask);
    long getPenaltyScore() const;
    std::vector<int> getAlignmentPatternPositions() const;

    static int getNumRawDataModules(int ver);
    static int getNumDataCodewords(int ver, Ecc ecl);
    static std::vector<uint8_t> reedSolomonComputeDivisor(int degree);
    static std::vector<uint8_t> reedSolomonComputeRemainder(const std::vector<uint8_t>& data,
                                                            const std::vector<uint8_t>& divisor);
    static uint8_t reedSolomonMultiply(uint8_t x, uint8_t y);
};

} // namespace qr
