// VyftTrade_market.h : paires listées sur Vyft Trade et cours de tous les actifs
//
// Listing et délisting par l'administrateur (X-ADMIN-KEY). Une paire BASE/COTATION (cotation VEZ par
// défaut) associe chaque actif à son jeton sur Slura : l'app crée pour chaque client un wallet VyftVEZ
// dont l'adresse du jeton est remplacée par celle de l'actif. Vyft Trade ne détient aucun fonds.
//
// Cours : le VEZ vaut réserve EUR ÷ offre (preuve de réserves) ; un actif listé prend le cours de sa
// source de référence (adresse web + chemin du champ dans la réponse JSON), en EUR ou en VEZ.

#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vyfttrade {

using json = nlohmann::json;

struct Asset {
    std::string symbol;     // « XAU », « VEZ »
    std::string name;       // « Or (once troy) »
    std::string token;      // adresse du jeton sur Slura (minuscules)
    int decimals = 18;
};

// Source du cours de référence : GET url, valeur au chemin `path` (« xau.eur », « data.0.price »)
struct IndexSource {
    std::string url;
    std::string path;
    std::string currency = "EUR";   // devise de la valeur lue : EUR ou VEZ
};

struct Pair {
    Asset base;
    Asset quote;
    std::optional<IndexSource> index;
    std::string status = "listed";   // listed | delisted
    std::string listedAt;
    std::string delistedAt;

    std::string symbol() const { return base.symbol + "/" + quote.symbol; }
};

// Jeton VEZ de Slura (cotation par défaut)
inline const Asset kVezAssetInfo{"VEZ", "Vyft VEZ", "0xeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee", 18};

json toJson(const Pair& pair);
json toJson(const Asset& asset);

// Lecture sur Slura nécessaire au listing : code présent à l'adresse, décimales du jeton
struct TokenProbe {
    std::function<bool(const std::string& token)> hasCode;
    std::function<std::optional<int>(const std::string& token)> decimals;
};

class MarketRegistry {
public:
    explicit MarketRegistry(std::filesystem::path file);

    std::vector<Pair> pairs(bool includeDelisted = false);
    std::optional<Pair> find(const std::string& symbol);   // « XAU/VEZ » ou « XAU-VEZ »

    // Listing : vérifie la demande et le jeton sur Slura ; renvoie le code HTTP et la paire ou l'erreur
    std::pair<int, json> list(const json& request, const TokenProbe& probe);
    std::pair<int, json> delist(const std::string& symbol);

private:
    void save();

    std::filesystem::path file_;
    std::vector<Pair> pairs_;
    std::mutex mutex_;
};

// Cours en EUR et en VEZ de tous les actifs listés (sources de référence, mises en cache)
class PriceBook {
public:
    using Fetch = std::function<std::optional<std::string>(const std::string& url)>;

    PriceBook(Fetch fetch, std::chrono::seconds ttl = std::chrono::seconds(15));

    // Cours d'1 actif dans une devise de référence ; nullopt si la source ne répond pas
    std::optional<double> indexValue(const IndexSource& source);

    // { "VEZ": { eur, vez: 1 }, "XAU": { eur, vez, pair, source }, … }
    json prices(MarketRegistry& registry, std::optional<double> vezEur);

private:
    struct Cached {
        double value = 0;
        std::chrono::steady_clock::time_point at;
    };
    Fetch fetch_;
    std::chrono::seconds ttl_;
    std::map<std::string, Cached> cache_;
    std::mutex mutex_;
};

// Valeur numérique au chemin « a.b.0.c » d'un document JSON (nombre ou texte numérique)
std::optional<double> jsonPathNumber(const json& document, const std::string& path);

}  // namespace vyfttrade
