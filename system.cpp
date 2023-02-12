#include <algorithm>
#include "system.hpp"

typedef std::promise<std::unique_ptr<Product>> product_promise_t;
typedef std::future<std::unique_ptr<Product>> product_future_t;

void OrderQueue::pushOrder(Order &value) {
    {
        std::unique_lock<std::mutex> lock(d_mutex);
        d_queue.push_front(std::move(value));
    }
    d_condition.notify_one();
}

std::pair<Order, std::vector<product_future_t>>
OrderQueue::manageOrder(OrderSynchronizer &sync,
                             std::unordered_map<std::string,
                                     std::shared_ptr<MachineWrapper>> &machines) {

    std::unique_lock<std::mutex> lock(d_mutex);

    d_condition.wait(lock, [=] { return !d_queue.empty(); });
    Order order(d_queue.back());
    d_queue.pop_back();

    auto futures = sync.insertOrderToMachines(order, machines);

    //todo może nie std::move
    return std::make_pair(std::move(order), futures);
}


void MachineQueue::pushPromise(product_promise_t &value) {
    {
        std::unique_lock<std::mutex> lock(d_mutex);
        d_queue.push_front(std::move(value));
    }
    d_condition.notify_one();
}


product_promise_t MachineQueue::popPromise() {
    std::unique_lock<std::mutex> lock(d_mutex);
    d_condition.wait(lock, [=] { return !d_queue.empty(); });
    product_promise_t promise(std::move(d_queue.back()));
    d_queue.pop_back();

    return promise;
}


void System::closeMachines() {
    for (auto &machine: machines) {
        machine.second->stopMachine();
    }
}


System::System(machines_t machines,
               unsigned int numberOfWorkers,
               unsigned int clientTimeout) :
        clientTimeout(clientTimeout),
        systemOpen(true) {

    //map all elements of machines to wrappers and initialize them
    for (auto &machine: machines) {
        this->machines.emplace(machine.first,
                               std::make_shared<MachineWrapper>(
                                       machine.first,
                                       machine.second,
                                       systemOpen));

    }

    for (auto &machine: machines) {
        menu.emplace(machine.first);
    }

    for (unsigned int i = 0; i < numberOfWorkers; i++) {
        orderWorkers.emplace_back([this]() { orderWorker(); });
    }
}


std::vector<WorkerReport> System::shutdown() {
    systemOpen = true;

    for (auto &w: orderWorkers) {
        if (w.joinable()) {
            w.join();
        }
    }

    closeMachines();
    std::vector<WorkerReport> dailyReport;

    for (auto &r: workerReports) {
        dailyReport.push_back(r);
    }

    return dailyReport;
}

std::vector<std::string> System::getMenu() const {
    return systemOpen ? std::vector<std::string>() :
           std::vector<std::string>(menu.begin(), menu.end());
}


std::vector<unsigned int> System::getPendingOrders() const {

    std::chrono::system_clock::time_point now
            = std::chrono::system_clock::now();
    std::vector<unsigned int> pendingOrders;

    for (const auto &element: orders) {
        auto order = element.second;
        if (order.checkIfPending()) {
            pendingOrders.push_back(order.getId());
        }
    }
    return pendingOrders;
}


std::unique_ptr<CoasterPager> System::order(std::vector<std::string> products) {
    if (systemOpen) {
        throw RestaurantClosedException();
    }

    if (!productsInMenu(products) || products.empty()) {
        throw BadOrderException();
    }

    size_t newId = idGenerator.newId();
    Order newOrder(newId, products, clientTimeout);
    orders.emplace(newId, newOrder);

    CoasterPager newPager(newOrder);
    pagers.emplace(newId, newPager);

    orderQueue.pushOrder(newOrder);

    return std::make_unique<CoasterPager>(newPager);
}


std::vector<std::unique_ptr<Product>>
System::collectOrder(std::unique_ptr<CoasterPager> CoasterPager) {
    auto orderStatus = CoasterPager->getOrder().getStatus();
    switch (orderStatus) {
        case Order::BROKEN_MACHINE: {
            throw FulfillmentFailure();
        }
        case Order::NOT_DONE: {
            throw OrderNotReadyException();
        }
        case Order::EXPIRED: {
            throw OrderExpiredException();
        }
        case Order::READY: {
            // Przypadki brzegowe itp.
            // zwrócenie produktu
            return {};
        }
        case Order::RECEIVED: {
            throw BadPagerException();
        }
        case Order::OTHER:
            throw BadPagerException();
    }
}


bool System::productsInMenu(std::vector<std::string> &products) {
    std::unique_lock<std::mutex> lock(menuMutex);
    return std::ranges::all_of(products.begin(),
                               products.end(),
                               [this](std::string &product) {
                                   return menu.contains(product);
                               });
}


void CoasterPager::wait() const {

}


void CoasterPager::wait(unsigned int timeout) const {

}

void System::orderWorker() {
    while (!systemOpen) {

        auto orderAndFutures = orderQueue.manageOrder(orderSynchronizer, machines);

        Order order = orderAndFutures.first;
        auto futureProducts = std::move(orderAndFutures.second);

        std::vector<std::unique_ptr<Product>> readyProducts;

        for(auto &futureProduct: futureProducts) {
            try {
                auto product = futureProduct.get();
                readyProducts.push_back(std::move(product));

            } catch (BadProductException &e) {

            }
        }
    }
}

void System::machineWorker() {

}

