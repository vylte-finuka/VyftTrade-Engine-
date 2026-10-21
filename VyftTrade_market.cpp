// VyftTrade_market.cpp : registre des paires (listing, délisting) et cours des actifs

#include "VyftTrade_market.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace vyfttrade {

namespace {

std::string nowUtc() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::string lower(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v;
}

std::string upper(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return v;
}

bool isAddress(const std::string& a) {
    return a.size() == 42 && a.rfind("0x", 0) == 0 &&
           std::all_of(a.begin() + 2, a.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

// Symbole d'actif : 2 à 12 caractères A-Z / 0-9
bool isSymbol(const std::string& s) {
    return s.size() >= 2 && s.size() <= 12 &&
           std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isupper(c) || std::isdigit(c); });
}

std::string text(const json& o, const char* key) {
    if (!o.is_object() || !o.contains(key)) return {};
    const json& v = o.at(key);
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number()) return v.dump();
    return {};
}

Asset assetFromJson(const json& j) {
    Asset a;
    a.symbol = j.value("symbol", "");
    a.name = j.value("name", a.symbol);
    a.token = lower(j.value("token", ""));
    a.decimals = j.value("decimals", 18);
    return a;
}

Pair pairFromJson(const json& j) {
    Pair p;
    p.base = assetFromJson(j.value("base", json::object()));
    p.quote = assetFromJson(j.value("quote", json::object()));
    if (j.contains("index") && j.at("index").is_object()) {
        const json& i = j.at("index");
        p.index = IndexSource{i.value("url", ""), i.value("path", ""), i.value("currency", "EUR")};
    }
    p.status = j.value("status", "listed");
    p.listedAt = j.value("listed_at", "");
    p.delistedAt = j.value("delisted_at", "");
    return p;
}

// « XAU/VEZ », « xau-vez » → « XAU/VEZ »
std::string normalizeSymbol(std::string s) {
    std::replace(s.begin(), s.end(), '-', '/');
    std::replace(s.begin(), s.end(), '_', '/');
    return upper(s);
}

}  // namespace

json toJson(const Asset& a) {
    return {{"symbol", a.symbol}, {"name", a.name}, {"token", a.token}, {"decimals", a.decimals}};
}

json toJson(const Pair& p) {
    json j = {
        {"symbol", p.symbol()},
        {"base", toJson(p.base)},
        {"quote", toJson(p.quote)},
        {"status", p.status},
        {"listed_at", p.listedAt},
    };
    if (p.index) j["index"] = {{"url", p.index->url}, {"path", p.index->path}, {"currency", p.index->currency}};
    if (!p.delistedAt.empty()) j["delisted_at"] = p.delistedAt;
    return j;
}

std::optional<double> jsonPathNumber(const json& document, const std::string& path) {
    const json* node = &document;
    std::stringstream parts(path);
    std::string key;
    while (std::getline(parts, key, '.')) {
        if (key.empty()) continue;
        if (node->is_array()) {
            if (!std::all_of(key.begin(), key.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) return std::nullopt;
            const size_t i = std::stoul(key);
            if (i >= node->size()) return std::nullopt;
            node = &(*node)[i];
        } else if (node->is_object() && node->contains(key)) {
            node = &node->at(key);
        } else {
            return std::nullopt;
        }
    }
    double value = 0;
    if (node->is_number()) value = node->get<double>();
    else if (node->is_string()) {
        try {
            value = std::stod(node->get<std::string>());
        } catch (...) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    return std::isfinite(value) && value > 0 ? std::optional<double>(value) : std::nullopt;
}

// ── Registre des paires ──

MarketRegistry::MarketRegistry(std::filesystem::path file) : file_(std::move(file)) {
    std::ifstream in(file_);
    if (!in) return;
    try {
        json data;
        in >> data;
        for (const auto& j : data.value("pairs", json::array())) pairs_.push_back(pairFromJson(j));
    } catch (...) {
        pairs_.clear();
    }
}

void MarketRegistry::save() {
    json data = {{"pairs", json::array()}};
    for (const auto& p : pairs_) data["pairs"].push_back(toJson(p));
    const auto tmp = file_.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << data.dump(2);
    }
    std::filesystem::rename(tmp, file_);
}

std::vector<Pair> MarketRegistry::pairs(bool includeDelisted) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Pair> out;
    for (const auto& p : pairs_)
        if (includeDelisted || p.status == "listed") out.push_back(p);
    return out;
}

std::optional<Pair> MarketRegistry::find(const std::string& symbol) {
    const std::string wanted = normalizeSymbol(symbol);
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& p : pairs_)
        if (p.symbol() == wanted) return p;
    return std::nullopt;
}

std::pair<int, json> MarketRegistry::list(const json& request, const TokenProbe& probe) {
    const json baseIn = request.value("base", json::object());
    Pair pair;
    pair.base.symbol = upper(text(baseIn, "symbol"));
    pair.base.name = text(baseIn, "name").empty() ? pair.base.symbol : text(baseIn, "name");
    pair.base.token = lower(text(baseIn, "token"));
    if (!isSymbol(pair.base.symbol)) return {400, {{"error", "base.symbol invalide (2 à 12 caractères A-Z, 0-9)"}}};
    if (!isAddress(pair.base.token)) return {400, {{"error", "base.token : adresse du jeton (0x + 40 caractères) requise"}}};

    // VEZ : listé comme les autres actifs, coté en EUR (paire de cours VEZ/EUR, source obligatoire)
    const json quoteIn = request.value("quote", json::object());
    const bool vezListing = pair.base.symbol == kVezAssetInfo.symbol || pair.base.token == kVezAssetInfo.token;
    if (vezListing) {
        if (pair.base.symbol != kVezAssetInfo.symbol || pair.base.token != kVezAssetInfo.token)
            return {400, {{"error", "Le VEZ se liste avec le symbole VEZ et le jeton " + kVezAssetInfo.token}}};
        if (!quoteIn.empty() && upper(text(quoteIn, "symbol")) != "EUR")
            return {400, {{"error", "Le VEZ est coté en EUR (paire VEZ/EUR)"}}};
        const json i = request.value("index", json::object());
        if (upper(text(i, "currency").empty() ? "EUR" : text(i, "currency")) != "EUR")
            return {400, {{"error", "index.currency : EUR pour le VEZ"}}};
        pair.quote = kEurAssetInfo;
    } else if (!quoteIn.empty() && upper(text(quoteIn, "symbol")) == "EUR") {
        return {400, {{"error", "Seul le VEZ est coté en EUR ; les autres actifs sont cotés en VEZ"}}};
    } else if (quoteIn.empty() || upper(text(quoteIn, "symbol")) == "VEZ") {
        pair.quote = kVezAssetInfo;
    } else {
        auto quote = find(upper(text(quoteIn, "symbol")) + "/VEZ");
        if (!quote || quote->status != "listed")
            return {400, {{"error", "La cotation doit être VEZ ou un actif déjà listé contre VEZ"}}};
        pair.quote = quote->base;
    }
    if (pair.base.symbol == pair.quote.symbol || pair.base.token == pair.quote.token)
        return {400, {{"error", "La base et la cotation doivent être deux actifs différents"}}};

    // Le jeton doit exister sur Slura ; décimales lues sur le jeton (ou données si illisibles)
    if (pair.base.token != kVezAssetInfo.token && !probe.hasCode(pair.base.token))
        return {422, {{"error", "Aucun contrat de jeton à cette adresse sur Slura"}}};
    const auto onChain = probe.decimals(pair.base.token);
    const bool given = baseIn.contains("decimals");
    const int declared = given ? baseIn.value("decimals", -1) : -1;
    if (onChain && given && *onChain != declared)
        return {422, {{"error", "Décimales différentes du jeton (" + std::to_string(*onChain) + " sur Slura)"}}};
    pair.base.decimals = onChain ? *onChain : declared;
    if (pair.base.decimals < 0 || pair.base.decimals > 36)
        return {422, {{"error", "Décimales du jeton illisibles : indiquez base.decimals"}}};

    if (request.contains("index") && request.at("index").is_object()) {
        const json& i = request.at("index");
        IndexSource source{text(i, "url"), text(i, "path"), upper(text(i, "currency").empty() ? "EUR" : text(i, "currency"))};
        if (source.url.rfind("https://", 0) != 0 || source.path.empty())
            return {400, {{"error", "index : url (https://…) et path (champ du cours) requis"}}};
        if (source.currency != "EUR" && source.currency != "VEZ")
            return {400, {{"error", "index.currency : EUR ou VEZ"}}};
        pair.index = source;
    }
    if (vezListing && !pair.index) return {400, {{"error", "index (url, path) requis : c'est la source du cours du VEZ"}}};

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& p : pairs_) {
        if (p.symbol() != pair.symbol()) continue;
        if (p.status == "listed") return {409, {{"error", "Paire déjà listée"}}};
        // Relisting : même identifiant, nouveaux réglages
        pair.listedAt = nowUtc();
        p = pair;
        save();
        return {201, toJson(p)};
    }
    for (const auto& p : pairs_)
        if (p.status == "listed" && p.base.symbol == pair.base.symbol && p.base.token != pair.base.token)
            return {409, {{"error", "Le symbole " + pair.base.symbol + " est déjà listé avec un autre jeton"}}};
    pair.listedAt = nowUtc();
    pairs_.push_back(pair);
    save();
    return {201, toJson(pair)};
}

std::pair<int, json> MarketRegistry::delist(const std::string& symbol) {
    const std::string wanted = normalizeSymbol(symbol);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& p : pairs_) {
        if (p.symbol() != wanted) continue;
        if (p.status != "listed") return {409, {{"error", "Paire déjà délistée"}}};
        for (const auto& other : pairs_)
            if (other.status == "listed" && other.quote.symbol == p.base.symbol && (p.quote.symbol == "VEZ" || p.quote.symbol == "EUR"))
                return {409, {{"error", "Délistez d'abord " + other.symbol() + ", cotée en " + p.base.symbol}}};
        p.status = "delisted";
        p.delistedAt = nowUtc();
        save();
        return {200, toJson(p)};
    }
    return {404, {{"error", "Paire introuvable"}}};
}

// ── Cours ──

PriceBook::PriceBook(Fetch fetch, std::chrono::seconds ttl) : fetch_(std::move(fetch)), ttl_(ttl) {}

std::optional<double> PriceBook::indexValue(const IndexSource& source) {
    const std::string key = source.url + "#" + source.path;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(key);
        if (it != cache_.end() && now - it->second.at < ttl_) return it->second.value;
    }
    auto body = fetch_(source.url);
    std::optional<double> value;
    if (body) {
        try {
            value = jsonPathNumber(json::parse(*body), source.path);
        } catch (...) {
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (value) cache_[key] = {*value, now};
    else if (auto it = cache_.find(key); it != cache_.end()) return it->second.value;   // dernier cours connu
    return value;
}

std::optional<double> PriceBook::vezEur(MarketRegistry& registry) {
    auto pair = registry.find(std::string(kVezAssetInfo.symbol) + "/" + kEurAssetInfo.symbol);
    if (!pair || pair->status != "listed" || !pair->index) return std::nullopt;
    return indexValue(*pair->index);
}

json PriceBook::prices(MarketRegistry& registry) {
    json out = json::object();
    const std::optional<double> vezEurValue = vezEur(registry);
    out["VEZ"] = {{"eur", vezEurValue ? json(*vezEurValue) : json(nullptr)}, {"vez", 1}};
    for (const auto& p : registry.pairs()) {
        if (p.base.symbol == kVezAssetInfo.symbol) {   // paire de cours VEZ/EUR
            out["VEZ"]["pair"] = p.symbol();
            if (p.index) out["VEZ"]["source"] = p.index->url;
            continue;
        }
        const std::optional<double>& vezEur = vezEurValue;
        json entry = {{"pair", p.symbol()}, {"eur", nullptr}, {"vez", nullptr}};
        if (p.index) {
            entry["source"] = p.index->url;
            if (auto v = indexValue(*p.index)) {
                // Cours de référence en EUR ou en VEZ, converti dans l'autre devise avec le cours du VEZ
                const double eur = p.index->currency == "EUR" ? *v : (vezEur ? *v * *vezEur : NAN);
                const double vez = p.index->currency == "VEZ" ? *v : (vezEur && *vezEur > 0 ? *v / *vezEur : NAN);
                if (std::isfinite(eur)) entry["eur"] = eur;
                if (std::isfinite(vez)) entry["vez"] = vez;
            }
        }
        out[p.base.symbol] = entry;
    }
    return out;
}

}  // namespace vyfttrade
