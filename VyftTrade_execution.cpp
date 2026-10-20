// VyftTrade_execution.cpp : exécution d'un ordre VEZ en ligne de commande
//
// Usage :
//   VyftTrade <buy|sell> <execute_order|cancel_order> <quantité> <wallet 0x…> [devise] [prix] [orderid]
//   VyftTrade --dry-run buy execute_order 12.5 0x… EUR 1      (affiche le JSON sans l'envoyer)
//   VyftTrade --leverage=4 --stop=0.001 buy execute_order 12.5 0x…   (position à effet de levier)
//   VyftTrade sell cancel_order 1 0x… EUR 1 vt_…              (clôture de la position vt_…)
//
// Clé d'API facultative : variable d'environnement VYFTTRADE_API_KEY (en-tête X-API-KEY)

#include "VyftTrade_motor.h"

#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    bool dryRun = false;
    double leverage = 1.0;
    double stopLoss = 0.0;   // 0 = stop automatique côté serveur
    while (!args.empty() && args.front().rfind("--", 0) == 0) {
        const std::string flag = args.front();
        if (flag == "--dry-run") dryRun = true;
        else if (flag.rfind("--leverage=", 0) == 0) leverage = std::strtod(flag.c_str() + 11, nullptr);
        else if (flag.rfind("--stop=", 0) == 0) stopLoss = std::strtod(flag.c_str() + 7, nullptr);
        else {
            std::cerr << "Option inconnue : " << flag << std::endl;
            return 1;
        }
        args.erase(args.begin());
    }

    if (args.size() < 4) {
        std::cerr << "Usage : VyftTrade [--dry-run] [--leverage=N] [--stop=PRIX] <buy|sell> <execute_order|cancel_order> <mise> "
                     "<wallet 0x...> [devise=EUR] [prix=1] [orderid]"
                  << std::endl;
        return 1;
    }

    auto trade = vyfttrade::parseTradeType(args[0]);
    auto action = vyfttrade::parseActionType(args[1]);
    if (!trade || !action) {
        std::cerr << "Type d'ordre invalide (buy|sell, execute_order|cancel_order)" << std::endl;
        return 1;
    }

    char* end = nullptr;
    const double quantity = std::strtod(args[2].c_str(), &end);
    if (end == args[2].c_str() || *end != '\0') {
        std::cerr << "Quantite invalide : " << args[2] << std::endl;
        return 1;
    }

    const std::string currency = args.size() > 4 ? args[4] : "EUR";
    const double price = args.size() > 5 ? std::strtod(args[5].c_str(), nullptr) : vyfttrade::kVezPriceEur;

    vyfttrade::Order order = vyfttrade::makeVezOrder(*trade, *action, quantity, args[3], currency, price);
    if (args.size() > 6) order.action.orderId = args[6];
    order.action.leverage = leverage;
    order.action.stopLoss = stopLoss;

    if (auto error = vyfttrade::validateOrder(order)) {
        std::cerr << *error << std::endl;
        return 1;
    }

    const std::string json = vyfttrade::buildOrderJson(order);
    if (dryRun) {
        std::cout << "POST " << vyfttrade::kTradeUrl << std::endl << json << std::endl;
        return 0;
    }

    const char* apiKey = std::getenv("VYFTTRADE_API_KEY");
    const vyfttrade::TradeResult result = vyfttrade::sendOrder(order, vyfttrade::kTradeUrl, apiKey ? apiKey : "");
    if (!result.ok) {
        std::cerr << result.error << std::endl;
        if (!result.body.empty()) std::cerr << result.body << std::endl;
        return result.httpStatus != 0 ? 3 : 2;
    }
    std::cout << result.body << std::endl;
    return 0;
}
