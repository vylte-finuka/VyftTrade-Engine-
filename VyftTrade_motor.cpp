// VyftTrade_motor.cpp : construction, validation et envoi des ordres Vyft Trade™ (VEZ / Slura)

#include "VyftTrade_motor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#elif defined(VYFTTRADE_WITH_CURL)
#include <curl/curl.h>
#endif

namespace vyfttrade {

namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// Échappement JSON des chaînes
std::string jsonString(const std::string& value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "\"";
}

std::string formatDecimal(double value, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        s.erase(s.find_last_not_of('0') + 1);
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

bool isHexAddress(const std::string& value) {
    if (value.size() != 42 || value[0] != '0' || (value[1] != 'x' && value[1] != 'X')) return false;
    return std::all_of(value.begin() + 2, value.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

bool isCurrencyCode(const std::string& value) {
    return value.size() == 3 &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isupper(c) != 0; });
}

#ifdef _WIN32
std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), size);
    return out;
}

// Requête HTTP(S) via WinHTTP (inclus dans Windows) ; body ignoré en GET
TradeResult request(const wchar_t* verb, const std::string& url, const std::string& body, const std::string& apiKey) {
    TradeResult result;
    std::wstring wurl = widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {};
    wchar_t path[1024] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) {
        result.error = "URL invalide : " + url;
        return result;
    }

    HINTERNET session = WinHttpOpen(L"VyftTradeEngine/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, verb, path, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, flags)
                                : nullptr;

    const bool isPost = std::wstring(verb) == L"POST";
    std::wstring headers = isPost ? L"Content-Type: application/json\r\nAccept: application/json\r\n"
                                  : L"Accept: application/json\r\n";
    if (!apiKey.empty()) headers += L"X-API-KEY: " + widen(apiKey) + L"\r\n";
    const DWORD length = isPost ? static_cast<DWORD>(body.size()) : 0;

    bool sent = request &&
                WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1L),
                                   isPost ? const_cast<char*>(body.data()) : WINHTTP_NO_REQUEST_DATA, length,
                                   length, 0) &&
                WinHttpReceiveResponse(request, nullptr);
    if (!sent) {
        result.error = "Erreur réseau WinHTTP (" + std::to_string(GetLastError()) + ")";
    } else {
        DWORD status = 0;
        DWORD size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        result.httpStatus = static_cast<long>(status);
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
            result.body.append(chunk.data(), read);
        }
        result.ok = status >= 200 && status < 300;
        if (!result.ok) result.error = "HTTP " + std::to_string(status);
    }

    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return result;
}
#elif defined(VYFTTRADE_WITH_CURL)
size_t appendBody(char* data, size_t size, size_t count, void* target) {
    static_cast<std::string*>(target)->append(data, size * count);
    return size * count;
}

// Requête HTTP(S) via libcurl (Linux / macOS) ; body ignoré en GET
TradeResult request(const char* verb, const std::string& url, const std::string& body, const std::string& apiKey) {
    TradeResult result;
    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error = "Initialisation libcurl impossible";
        return result;
    }
    const bool isPost = std::string(verb) == "POST";
    curl_slist* headers = nullptr;
    if (isPost) headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    std::string keyHeader = "X-API-KEY: " + apiKey;
    if (!apiKey.empty()) headers = curl_slist_append(headers, keyHeader.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (isPost) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode code = curl_easy_perform(curl);
    if (code != CURLE_OK) {
        result.error = std::string("Erreur réseau : ") + curl_easy_strerror(code);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.httpStatus);
        result.ok = result.httpStatus >= 200 && result.httpStatus < 300;
        if (!result.ok) result.error = "HTTP " + std::to_string(result.httpStatus);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}
#else
TradeResult request(const char*, const std::string&, const std::string&, const std::string&) {
    TradeResult result;
    result.error = "Aucun client HTTP : compilez avec libcurl (VYFTTRADE_WITH_CURL) hors Windows";
    return result;
}
#endif

#ifdef _WIN32
#define VYFTTRADE_VERB(v) L##v
#else
#define VYFTTRADE_VERB(v) v
#endif

// Envoi d'un ordre : message d'erreur explicite en cas de refus de l'API
TradeResult postJson(const std::string& url, const std::string& body, const std::string& apiKey) {
    TradeResult result = request(VYFTTRADE_VERB("POST"), url, body, apiKey);
    if (!result.ok && result.httpStatus != 0)
        result.error = "Ordre refusé par Vyft Trade (HTTP " + std::to_string(result.httpStatus) + ")";
    return result;
}

}  // namespace

std::optional<TradeType> parseTradeType(const std::string& value) {
    const std::string v = lower(value);
    if (v == "buy") return TradeType::Buy;
    if (v == "sell") return TradeType::Sell;
    return std::nullopt;
}

std::optional<ActionType> parseActionType(const std::string& value) {
    const std::string v = lower(value);
    if (v == "execute_order" || v == "order" || v == "execute") return ActionType::ExecuteOrder;
    if (v == "cancel_order" || v == "cancel") return ActionType::CancelOrder;
    return std::nullopt;
}

const char* toString(TradeType value) { return value == TradeType::Buy ? "buy" : "sell"; }

const char* toString(ActionType value) {
    return value == ActionType::ExecuteOrder ? "execute_order" : "cancel_order";
}

Order makeVezOrder(TradeType tradeType, ActionType actionType, double quantity, const std::string& wallet,
                   const std::string& currency, double priceInCurrency) {
    Order order;
    order.action.tradeType = tradeType;
    order.action.actionType = actionType;
    order.action.quantity = quantity;
    order.action.wallet = wallet;
    order.action.currency = currency;
    order.action.priceInCurrency = priceInCurrency;
    return order;
}

std::string formatQuantity(double quantity) { return formatDecimal(quantity, kVezDecimals); }

std::optional<std::string> validateOrder(const Order& order) {
    const auto& a = order.action;
    if (order.assetData.asset != kVezAsset) return std::string("Seul le VEZ est négociable sur Vyft Trade");
    if (order.assetData.chain != kVezChain) return std::string("Le VEZ se règle sur la chaîne Slura (SLU)");
    if (!std::isfinite(a.quantity) || a.quantity <= 0) return std::string("Quantité invalide");
    const double unit = std::pow(10.0, -kVezDecimals);
    if (a.quantity < unit) return "Quantité minimale : " + formatQuantity(unit) + " VEZ";
    if (!isCurrencyCode(a.currency)) return std::string("Devise invalide (code ISO à 3 lettres, ex. EUR)");
    if (!std::isfinite(a.priceInCurrency) || a.priceInCurrency <= 0) return std::string("Prix invalide");
    if (!isHexAddress(a.wallet)) return std::string("Adresse de sous-wallet VEZ invalide (0x + 40 caractères)");
    if (a.actionType == ActionType::CancelOrder && a.orderId.empty())
        return std::string("Identifiant d'ordre requis pour l'annulation");
    if (!std::isfinite(a.leverage) || a.leverage < 1) return std::string("Levier invalide (1 minimum)");
    if (!std::isfinite(a.stopLoss) || a.stopLoss < 0) return std::string("Stop loss invalide");
    return std::nullopt;
}

TradeResult httpGet(const std::string& url) { return request(VYFTTRADE_VERB("GET"), url, "", ""); }

TradeResult httpPostJson(const std::string& url, const std::string& body, const std::string& apiKey) {
    return request(VYFTTRADE_VERB("POST"), url, body, apiKey);
}

double maxLeverage(double stake, double availableVez, double maxTransferVez) {
    if (!std::isfinite(stake) || stake <= 0 || !std::isfinite(availableVez) || availableVez <= 0) return 0;
    // Le versement net d'une position ne peut dépasser ni l'offre disponible, ni le plafond du contrat
    double payoutLimit = availableVez;
    if (std::isfinite(maxTransferVez) && maxTransferVez > 0) payoutLimit = std::min(payoutLimit, maxTransferVez);
    return std::floor(payoutLimit / maxPayout(stake, 1.0));
}

double maxGain(double stake, double leverage) { return stake * leverage * kGainCapMove; }

double maxPayout(double stake, double leverage) { return maxGain(stake, leverage) * (1.0 - kProfitFeeMin); }

double profitFeeRate(double health, double utilization) {
    auto clamp01 = [](double v) { return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 1.0; };
    const double stress = std::max(1.0 - clamp01(health), clamp01(utilization));
    return kProfitFeeMin + (kProfitFeeMax - kProfitFeeMin) * stress;
}

ProfitSplit splitProfit(double gross, double feeRate) {
    const double unit = std::pow(10.0, kVezDecimals);
    ProfitSplit split;
    split.gross = std::isfinite(gross) && gross > 0 ? std::floor(gross * unit) / unit : 0;
    split.feeRate = std::clamp(std::isfinite(feeRate) ? feeRate : kProfitFeeMax, kProfitFeeMin, kProfitFeeMax);
    // Le net est arrondi vers le bas : l'arrondi profite au pool
    split.net = std::floor(split.gross * (1.0 - split.feeRate) * unit) / unit;
    split.fee = std::round((split.gross - split.net) * unit) / unit;
    return split;
}

double autoStopLoss(TradeType side, double entryPrice, double leverage) {
    return side == TradeType::Buy ? entryPrice * (1.0 - 1.0 / leverage) : entryPrice * (1.0 + 1.0 / leverage);
}

std::optional<std::string> validatePosition(const Position& p, double availableVez, double maxTransferVez) {
    if (!std::isfinite(p.stake) || p.stake <= 0) return std::string("Mise invalide");
    if (!std::isfinite(p.entryPrice) || p.entryPrice <= 0) return std::string("Prix VEZ indisponible");
    if (!std::isfinite(p.leverage) || p.leverage < 1) return std::string("Levier invalide (1 minimum)");
    if (p.side == TradeType::Buy && !(p.stopLoss >= 0 && p.stopLoss < p.entryPrice))
        return std::string("Le stop loss d'une position à la hausse doit être sous le prix d'entrée");
    if (p.side == TradeType::Sell && !(p.stopLoss > p.entryPrice))
        return std::string("Le stop loss d'une position à la baisse doit être au-dessus du prix d'entrée");
    const double limit = maxLeverage(p.stake, availableVez, maxTransferVez);
    if (limit < 1) return std::string("Offre du pool insuffisante pour couvrir cette mise");
    if (p.leverage > limit) return "Levier maximum pour cette mise : x" + formatDecimal(limit, 0);
    return std::nullopt;
}

double positionResult(const Position& p, double price) {
    const double move = p.side == TradeType::Buy ? price / p.entryPrice - 1.0 : 1.0 - price / p.entryPrice;
    return p.stake * p.leverage * move;
}

CloseReason checkClose(const Position& p, double price) {
    if (!std::isfinite(price) || price <= 0) return CloseReason::None;
    if (p.side == TradeType::Buy) {
        if (p.stopLoss > 0 && price <= p.stopLoss) return CloseReason::StopLoss;
        if (price >= p.entryPrice * (1.0 + kGainCapMove)) return CloseReason::TakeProfit;
    } else {
        if (price >= p.stopLoss) return CloseReason::StopLoss;
        if (price <= p.entryPrice * (1.0 - kGainCapMove)) return CloseReason::TakeProfit;
    }
    return CloseReason::None;
}

double settlementAmount(const Position& p, double price, CloseReason reason) {
    if (reason == CloseReason::StopLoss) return 0;
    const double result = positionResult(p, price);
    if (!std::isfinite(result) || result <= 0) return 0;
    return std::floor(std::min(result, maxGain(p.stake, p.leverage)) * std::pow(10.0, kVezDecimals)) /
           std::pow(10.0, kVezDecimals);
}

const char* toString(CloseReason value) {
    switch (value) {
        case CloseReason::StopLoss: return "stop_loss";
        case CloseReason::TakeProfit: return "take_profit";
        default: return "manual";
    }
}

std::string buildOrderJson(const Order& order) {
    const auto& d = order.assetData;
    const auto& a = order.action;
    std::ostringstream json;
    json << "{\"assetdata\":{"
         << "\"assetname\":" << jsonString(d.assetName) << ","
         << "\"asset\":" << jsonString(d.asset) << ","
         << "\"chain\":" << jsonString(d.chain) << "},"
         << "\"action\":{"
         << "\"typetrade\":" << jsonString(toString(a.tradeType)) << ","
         << "\"typeaction\":" << jsonString(toString(a.actionType)) << ","
         << "\"quantity\":" << jsonString(formatQuantity(a.quantity)) << ","
         << "\"currency\":" << jsonString(a.currency) << ","
         << "\"priceincurrency\":" << jsonString(formatDecimal(a.priceInCurrency, 8)) << ","
         << "\"wallet\":" << jsonString(a.wallet) << ","
         << "\"leverage\":" << jsonString(formatDecimal(a.leverage, 2));
    if (a.stopLoss > 0) json << ",\"stoploss\":" << jsonString(formatDecimal(a.stopLoss, 10));
    if (!a.orderId.empty()) json << ",\"orderid\":" << jsonString(a.orderId);
    json << "}}";
    return json.str();
}

TradeResult sendOrder(const Order& order, const std::string& url, const std::string& apiKey) {
    if (auto error = validateOrder(order)) {
        TradeResult result;
        result.error = *error;
        return result;
    }
    return postJson(url, buildOrderJson(order), apiKey);
}

}  // namespace vyfttrade

namespace {
int runOrder(const vyfttrade::Order& order) {
    const vyfttrade::TradeResult result = vyfttrade::sendOrder(order);
    if (result.ok) {
        std::cout << result.body << std::endl;
        return 0;
    }
    std::cerr << result.error << std::endl;
    if (!result.body.empty()) std::cerr << result.body << std::endl;
    if (result.httpStatus != 0) return 3;
    return vyfttrade::validateOrder(order) ? 1 : 2;
}
}  // namespace

extern "C" {

int Tradelive(const char* assetName, const char* tradeType, const char* actionType, double quantity,
              const char* currency, double priceInCurrency, const char* wallet) {
    if (!assetName || !tradeType || !actionType || !currency || !wallet) return 1;
    auto trade = vyfttrade::parseTradeType(tradeType);
    auto action = vyfttrade::parseActionType(actionType);
    if (!trade || !action) return 1;
    vyfttrade::Order order = vyfttrade::makeVezOrder(*trade, *action, quantity, wallet, currency, priceInCurrency);
    order.assetData.asset = assetName;   // refusé par validateOrder s'il ne s'agit pas du VEZ
    return runOrder(order);
}

int TradeliveVez(const char* tradeType, const char* actionType, double quantity, const char* wallet) {
    return Tradelive(vyfttrade::kVezAsset, tradeType, actionType, quantity, "EUR", vyfttrade::kVezPriceEur,
                     wallet);
}
}
