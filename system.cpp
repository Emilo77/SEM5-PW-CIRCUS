#include <algorithm>
#include "system.hpp"

void System::initializeMachines(machines_t &m) {
    for (auto &machine: m) {
        machineStatus.emplace(machine.first, MachineStatus::OFF);
    }
}


void System::closeMachines() {
    for (auto &machine: machines) {
        machine.second->stop();
    }
}


System::System(machines_t machines,
               unsigned int numberOfWorkers,
               unsigned int clientTimeout) :
        numberOfWorkers(numberOfWorkers),
        clientTimeout(clientTimeout),
        closed(false),
        machines(std::move(machines)) {

    initializeMachines(machines);

    for (auto &machine: machines) {
        menu.emplace(machine.first);
    }

    for (unsigned int i = 0; i < numberOfWorkers; i++) {
        workers.emplace_back([this]() { startWorking(); });
    }
}


std::vector<WorkerReport> System::shutdown() {
    closed = true;

    for (auto &w: workers) {
        if (w.joinable()) {
            w.join();
        }
    }

    closeMachines();
    std::vector<WorkerReport> dailyReport;

    for (auto &r: reports) {
        dailyReport.push_back(r);
    }

    return dailyReport;
}


std::vector<unsigned int> System::getPendingOrders() const {
    std::chrono::system_clock::time_point now
            = std::chrono::system_clock::now();
    std::vector<unsigned int> pendingOrders;
    for (auto order: orders) {
        if (order.pendingId(now).has_value()) {
            pendingOrders.push_back(order.getId());
        }
    }
    return pendingOrders;
}


std::unique_ptr<CoasterPager> System::order(std::vector<std::string> products) {
    if (closed) {
        throw RestaurantClosedException();
    }

    if (!productsInMenu(products)) {
        throw BadOrderException();
    }


    //stworzenie pagera i zwrócenie go

    return std::unique_ptr<CoasterPager>();
}


std::vector<std::unique_ptr<Product>>
System::collectOrder(std::unique_ptr<CoasterPager> CoasterPager) {
    return std::vector<std::unique_ptr<Product>>();
}


bool System::productsInMenu(std::vector<std::string> &products) {
    return std::ranges::all_of(products.begin(),
                               products.end(),
                               [this](std::string &product) { return menu.contains(product); });
}

void System::startWorking() {
    while (!closed) {

    }
}


void CoasterPager::wait() const {

}


void CoasterPager::wait(unsigned int timeout) const {
    sleep(timeout);
}


bool CoasterPager::isReady() const {
    return false;
}
