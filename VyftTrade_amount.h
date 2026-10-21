// VyftTrade_amount.h : montants exacts en unités de jeton (entiers non signés 512 bits)
//
// Les montants sur la chaîne sont des uint256 (VEZ : 18 décimales). Les calculs du carnet
// (quantité × prix) passent par 512 bits pour ne jamais déborder ni arrondir en double.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace vyfttrade {

class U512 {
public:
    static constexpr int kWords = 8;   // 8 × 64 bits, mot 0 = poids faible

    U512() { w_.fill(0); }
    U512(uint64_t v) { w_.fill(0); w_[0] = v; }   // NOLINT : conversion voulue

    static U512 pow10(int n) {
        U512 r(1);
        for (int i = 0; i < n; ++i) r = r * U512(10);
        return r;
    }

    bool isZero() const {
        for (auto v : w_)
            if (v) return false;
        return true;
    }

    // Tient dans un uint256 (encodage ABI)
    bool fits256() const { return !w_[4] && !w_[5] && !w_[6] && !w_[7]; }

    friend bool operator==(const U512& a, const U512& b) { return a.w_ == b.w_; }
    friend bool operator!=(const U512& a, const U512& b) { return !(a == b); }
    friend bool operator<(const U512& a, const U512& b) {
        for (int i = kWords - 1; i >= 0; --i)
            if (a.w_[i] != b.w_[i]) return a.w_[i] < b.w_[i];
        return false;
    }
    friend bool operator>(const U512& a, const U512& b) { return b < a; }
    friend bool operator<=(const U512& a, const U512& b) { return !(b < a); }
    friend bool operator>=(const U512& a, const U512& b) { return !(a < b); }

    friend U512 operator+(const U512& a, const U512& b) {
        U512 r;
        unsigned __int128 carry = 0;
        for (int i = 0; i < kWords; ++i) {
            const unsigned __int128 s = static_cast<unsigned __int128>(a.w_[i]) + b.w_[i] + carry;
            r.w_[i] = static_cast<uint64_t>(s);
            carry = s >> 64;
        }
        if (carry) throw std::overflow_error("U512 : dépassement en addition");
        return r;
    }

    friend U512 operator-(const U512& a, const U512& b) {
        if (a < b) throw std::underflow_error("U512 : soustraction négative");
        U512 r;
        uint64_t borrow = 0;
        for (int i = 0; i < kWords; ++i) {
            const uint64_t ai = a.w_[i];
            const uint64_t d = ai - b.w_[i] - borrow;
            borrow = (ai < b.w_[i] || (ai == b.w_[i] && borrow)) ? 1 : 0;
            r.w_[i] = d;
        }
        return r;
    }

    friend U512 operator*(const U512& a, const U512& b) {
        std::array<uint64_t, kWords * 2> t{};
        for (int i = 0; i < kWords; ++i) {
            if (!a.w_[i]) continue;
            unsigned __int128 carry = 0;
            for (int j = 0; j < kWords; ++j) {
                const unsigned __int128 cur =
                    static_cast<unsigned __int128>(a.w_[i]) * b.w_[j] + t[i + j] + carry;
                t[i + j] = static_cast<uint64_t>(cur);
                carry = cur >> 64;
            }
            for (int k = i + kWords; carry && k < kWords * 2; ++k) {
                const unsigned __int128 cur = static_cast<unsigned __int128>(t[k]) + carry;
                t[k] = static_cast<uint64_t>(cur);
                carry = cur >> 64;
            }
        }
        for (int i = kWords; i < kWords * 2; ++i)
            if (t[i]) throw std::overflow_error("U512 : dépassement en multiplication");
        U512 r;
        for (int i = 0; i < kWords; ++i) r.w_[i] = t[i];
        return r;
    }

    // Division entière (quotient, reste) par décalages successifs
    static std::pair<U512, U512> divmod(const U512& a, const U512& b) {
        if (b.isZero()) throw std::domain_error("U512 : division par zéro");
        U512 q, r;
        for (int bit = kWords * 64 - 1; bit >= 0; --bit) {
            r = r.shl1();
            if ((a.w_[bit / 64] >> (bit % 64)) & 1) r.w_[0] |= 1;
            if (r >= b) {
                r = r - b;
                q.w_[bit / 64] |= (uint64_t{1} << (bit % 64));
            }
        }
        return {q, r};
    }
    friend U512 operator/(const U512& a, const U512& b) { return divmod(a, b).first; }
    friend U512 operator%(const U512& a, const U512& b) { return divmod(a, b).second; }

    // a × b ÷ c, arrondi vers le bas ou vers le haut
    static U512 mulDiv(const U512& a, const U512& b, const U512& c, bool roundUp = false) {
        auto [q, r] = divmod(a * b, c);
        return roundUp && !r.isZero() ? q + U512(1) : q;
    }

    static U512 min(const U512& a, const U512& b) { return a < b ? a : b; }

    // Entier décimal ("123456") → U512 ; nullopt si invalide
    static std::optional<U512> parseInteger(const std::string& s) {
        if (s.empty() || s.size() > 150) return std::nullopt;
        U512 r;
        for (char c : s) {
            if (c < '0' || c > '9') return std::nullopt;
            r = r * U512(10) + U512(static_cast<uint64_t>(c - '0'));
        }
        return r;
    }

    // Montant décimal ("12.5") → unités (12.5 × 10^decimals) ; nullopt si invalide ou trop précis
    static std::optional<U512> parseUnits(const std::string& s, int decimals) {
        if (s.empty() || decimals < 0 || decimals > 36) return std::nullopt;
        const auto dot = s.find('.');
        std::string whole = dot == std::string::npos ? s : s.substr(0, dot);
        std::string frac = dot == std::string::npos ? "" : s.substr(dot + 1);
        if (whole.empty()) whole = "0";
        while (frac.size() > static_cast<size_t>(decimals)) {
            if (frac.back() != '0') return std::nullopt;   // plus de décimales que le jeton
            frac.pop_back();
        }
        frac.append(static_cast<size_t>(decimals) - frac.size(), '0');
        return parseInteger(whole + frac);
    }

    std::string toDecimalString() const {
        if (isZero()) return "0";
        std::string out;
        U512 v = *this;
        const U512 ten(10);
        while (!v.isZero()) {
            auto [q, r] = divmod(v, ten);
            out.insert(out.begin(), static_cast<char>('0' + r.w_[0]));
            v = q;
        }
        return out;
    }

    // Unités → montant décimal sans zéros inutiles ("12.5")
    std::string formatUnits(int decimals) const {
        std::string digits = toDecimalString();
        if (decimals <= 0) return digits;
        if (digits.size() <= static_cast<size_t>(decimals))
            digits.insert(0, static_cast<size_t>(decimals) - digits.size() + 1, '0');
        std::string whole = digits.substr(0, digits.size() - decimals);
        std::string frac = digits.substr(digits.size() - decimals);
        while (!frac.empty() && frac.back() == '0') frac.pop_back();
        return frac.empty() ? whole : whole + "." + frac;
    }

    // Approximation pour l'affichage et les statistiques (jamais pour les règlements)
    long double toLongDouble(int decimals = 0) const {
        long double r = 0;
        for (int i = kWords - 1; i >= 0; --i) r = r * 18446744073709551616.0L + static_cast<long double>(w_[i]);
        for (int i = 0; i < decimals; ++i) r /= 10;
        return r;
    }

    // Mot ABI big-endian de 32 octets (exige fits256)
    std::array<uint8_t, 32> toWord() const {
        if (!fits256()) throw std::overflow_error("U512 : valeur au-delà de uint256");
        std::array<uint8_t, 32> out{};
        for (int i = 0; i < 4; ++i)
            for (int b = 0; b < 8; ++b) out[31 - (i * 8 + b)] = static_cast<uint8_t>(w_[i] >> (8 * b));
        return out;
    }

    // Hexadécimal ("0x…") → U512
    static std::optional<U512> parseHex(const std::string& s) {
        size_t i = s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0 ? 2 : 0;
        if (s.size() - i > 128) return std::nullopt;
        U512 r;
        for (; i < s.size(); ++i) {
            const char c = s[i];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return std::nullopt;
            r = r * U512(16) + U512(static_cast<uint64_t>(d));
        }
        return r;
    }

private:
    U512 shl1() const {
        U512 r;
        for (int i = kWords - 1; i >= 0; --i) r.w_[i] = (w_[i] << 1) | (i ? (w_[i - 1] >> 63) : 0);
        return r;
    }

    std::array<uint64_t, kWords> w_;
};

}  // namespace vyfttrade
