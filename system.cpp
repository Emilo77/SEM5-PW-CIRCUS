#include <algorithm>
#include "system.hpp"

typedef std::promise<std::unique_ptr<Product>> product_promise_t;
typedef std::future<std::unique_ptr<Product>> product_future_t;
typedef std::unordered_map<std::string,
        std::shared_ptr<MachineWrapper>> machine_wrappers_t;

void Order::waitForOrder() {
    std::unique_lock<std::mutex> lock(*orderInfoMutex);
    clientNotifier->wait(lock, [=] { return status != NOT_DONE; });
    if (status != READY) {
        throw FulfillmentFailure();
    }
}

void Order::waitForOrderTimed(unsigned int time) {
    std::unique_lock<std::mutex> lock(*orderInfoMutex);
    clientNotifier->wait_for(lock, std::chrono::milliseconds(time), [=] {
        return status != NOT_DONE;
    });
    if (status != READY) {
        throw FulfillmentFailure();
    }
}

void Order::waitForClient(WorkerReportUpdater &infoUpdater,
                          Order::machine_wrappers_t &machines) {
    std::unique_lock<std::mutex> lock(*orderInfoMutex);

    auto result = workerNotifier->wait_for(lock, std::chrono::milliseconds(
            timeout));
    if (result == std::cv_status::timeout) {
        /* Klient pojawił się na czas i odebrał produkty. */
        setStatus(Order::RECEIVED);
        infoUpdater.updateOrder(*this, WorkerReportUpdater::COLLECT);
    } else {
        /* Jeżeli klient nie pojawił się na czas, zmienienie statusu
                 * zamówienia i zwrócenie produktów do maszyn. */
        setStatus(Order::EXPIRED);
        infoUpdater.updateOrder(*this, WorkerReportUpdater::ABANDON);
        returnProducts(machines);
    }
}

void
Order::markDone(std::vector<std::pair<std::string, std::unique_ptr<Product>>>
                &namedProducts) {
    /* Dodanie produktów do zamówienia, zaktualizowanie statusu. */
    {
        std::unique_lock<std::mutex> lock(*orderInfoMutex);
        readyProducts = std::move(namedProducts);
        doneTime = std::chrono::system_clock::now();
        status = READY;
    }
    /* Powiadomienie klienta czekającego na funkcjach z pager'a. */
}

std::vector<std::unique_ptr<Product>> Order::collectReadyProducts() {
    std::unique_lock<std::mutex> lock(*orderInfoMutex);

    /* Wyciągnięcie gotowych produktów, ich nazwy są już niepotrzebne. */
    std::vector<std::unique_ptr<Product>> achievedProducts;
    for (auto &namedProduct: readyProducts) {
        achievedProducts.push_back(std::move(namedProduct.second));
    }
    /* Wyczyszczenie kontenera na produkty. */
    readyProducts.clear();

    /* Zwrócenie gotowych produktów. */
    return achievedProducts;
}

void Order::returnProducts(machine_wrappers_t &machines) {
    for (auto &product: readyProducts) {
        machines.at(product.first)->returnProduct(product.second);
    }
}

size_t Order::getId() const {
    std::unique_lock<std::mutex> lock(*orderInfoMutex);
    return id;
}

std::vector<std::string> Order::getProductNames() {
    std::unique_lock<std::mutex> lock(*orderInfoMutex);
    return products;
}

bool Order::isReady() {
    std::unique_lock<std::mutex> lock(*orderInfoMutex);
    updateIfExpired();
    return status == READY;
}

bool Order::checkIfPending() {
    std::unique_lock<std::mutex> lock(*orderInfoMutex);
    updateIfExpired();
    if (status == READY || status == NOT_DONE) {
        return true;
    }
    return false;
}

Order::OrderStatus Order::getStatus() {
    std::unique_lock<std::mutex> lock(*orderInfoMutex);
    updateIfExpired();
    return status;
}

void Order::setStatus(OrderStatus newStatus) {
    std::unique_lock<std::mutex> lock(*orderInfoMutex);
    status = newStatus;
}

void Order::updateIfExpired() {
    if (status == READY) {
        std::chrono::duration<double> elapsedTime =
                std::chrono::system_clock::now() - doneTime;
        if (elapsedTime > std::chrono::milliseconds(timeout)) {
            status = EXPIRED;
        }
    }
}

void WorkerReportUpdater::updateOrder(Order &order, ACTION a) {
    switch (a) {
        case FAIL:
            report.failedOrders.push_back(order.getProductNames());
            break;
        case COLLECT:
            report.collectedOrders.push_back(order.getProductNames());
            break;
        case ABANDON:
            report.abandonedOrders.push_back(order.getProductNames());
            break;
    }
}


void OrderQueue::pushOrder(std::shared_ptr<Order> &order) {
    {
        std::unique_lock<std::mutex> lock(d_mutex);
        d_queue.push_back(order);
    }
    d_condition.notify_one();
}

std::shared_ptr<Order>
OrderQueue::popOrder(std::unique_lock<std::mutex> &lock) {
    /* Zakładamy, że lock zostanie ustawiony przed wywołaniem funkcji. */
    d_condition.wait(lock, [=] { return !d_queue.empty(); });
    std::shared_ptr<Order> order(std::move(d_queue.back()));
    d_queue.pop_front();

    return order;
}

bool OrderQueue::isEmpty() {
    std::unique_lock<std::mutex> lock(d_mutex);
    return d_queue.empty();
}


void MachineQueue::pushPromise(product_promise_t &value) {
    {
        std::unique_lock<std::mutex> lock(d_mutex);
        d_queue.push_back(std::move(value));
    }
    d_condition.notify_one();
}


product_promise_t MachineQueue::popPromise(std::atomic_bool
                                           &orderWorkersEnded) {
    std::unique_lock<std::mutex> lock(d_mutex);
    d_condition.wait(lock, [this, &orderWorkersEnded] {
        return !d_queue.empty() || orderWorkersEnded;
    });
    if (orderWorkersEnded) {
        return {};
    }
    product_promise_t promise(std::move(d_queue.back()));
    d_queue.pop_front();

    return promise;
}

void System::removeFromMenu(std::string &product) {
    std::unique_lock<std::mutex> lock(menuMutex);
    auto it = std::find(menu.begin(), menu.end(), product);
    if (it != menu.end()) {
        menu.erase(it);
    }
}


System::System(machines_t machines,
               unsigned int numberOfWorkers,
               unsigned int clientTimeout) :
        clientTimeout(clientTimeout),
        systemOpen(true),
        orderWorkersEnded(false) {

    //map all elements of machines to wrappers and initialize them
    for (auto &machine: machines) {
        this->machines.emplace(machine.first,
                               std::make_shared<MachineWrapper>(
                                       machine.first,
                                       machine.second,
                                       orderWorkersEnded));

    }

    for (auto &machine: machines) {
        menu.emplace(machine.first);
    }

    for (unsigned int i = 0; i < numberOfWorkers; i++) {
        orderWorkers.emplace_back([this]() { orderWorker(); });
    }
}


std::vector<WorkerReport> System::shutdown() {
    systemOpen = false;

    for (auto &w: orderWorkers) {
        if (w.joinable()) {
            w.join();
        }
    }

    orderWorkersEnded = true;

    // todo może to by się udało jakoś zrobić w destruktorze MachineWrapper
    for (auto &element: machines) {
        element.second->joinMachineWorker();
    }

    return workerReports;
}

std::vector<std::string> System::getMenu() const {
    return !systemOpen ? std::vector<std::string>() :
           std::vector<std::string>(menu.begin(), menu.end());
}


std::vector<unsigned int> System::getPendingOrders() const {
    std::vector<unsigned int> pendingOrders;

    for (auto &element: orders) {
        auto order = element.second;
        if (order->checkIfPending()) {
            pendingOrders.push_back(order->getId());
        }
    }

    return pendingOrders;
}


std::unique_ptr<CoasterPager> System::order(std::vector<std::string> products) {
    if (!systemOpen) {
        throw RestaurantClosedException();
    }

    if (products.empty() || !productsInMenu(products)) {
        throw BadOrderException();
    }

    size_t newId = idGenerator.newId();
    auto newOrder = std::make_shared<Order>(
            Order(newId, products, clientTimeout));

    orders.emplace(newId, newOrder);
    orderQueue.pushOrder(newOrder);

    return std::make_unique<CoasterPager>(CoasterPager(newOrder));
}


std::vector<std::unique_ptr<Product>>
System::collectOrder(std::unique_ptr<CoasterPager> pager) {
    auto orderStatus = pager->getOrder()->getStatus();
    switch (orderStatus) {
        case Order::BROKEN_MACHINE:
            throw FulfillmentFailure();
        case Order::NOT_DONE:
            throw OrderNotReadyException();
        case Order::EXPIRED:
            throw OrderExpiredException();
        case Order::RECEIVED:
        case Order::OTHER:
            throw BadPagerException();
        case Order::READY: {
            pager->getOrder()->notifyWorker();
            auto readyProducts = pager->getOrder()->collectReadyProducts();
            /* Jeżeli produktów nie ma, klient jednak nie zdążył. */
            if (readyProducts.empty()) {
                throw OrderExpiredException();
            } else {
                /* Zwrócenie produktów klientowi. */
                return readyProducts;
            }
        }
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


void System::orderWorker() {
    WorkerReportUpdater infoUpdater;

    /* Pracownik wykonuje pracę, dopóki system nie zamknie się i kolejka nie
     * będzie pusta. */
    while (systemOpen || !orderQueue.isEmpty()) {

        /* Zablokowanie kolejki zamówień. */
        std::unique_lock<std::mutex> lock(orderQueue.getMutex());

        /* Odebranie zamówienia z kolejki blokującej. */
        auto orderPtr = orderQueue.popOrder(lock);

        /* Zlecenie maszynom wyprodukowanie produktów. */
        auto futureProducts = orderSynchronizer.insertOrderToMachines(*orderPtr,

                                                                      machines);
        /* Odblokowanie kolejki zamówień. */
        lock.unlock();

        /* Wyprodukowane produkty przez maszyny. */
        std::vector<std::pair<std::string, std::unique_ptr<Product>>>
                readyProducts;

        for (auto &futureProduct: futureProducts) {
            std::string productName = futureProduct.first;
            try {
                auto product = futureProduct.second.get();

                /* Jeżeli wyprodukowanie powiodło się, dodanie do kontenera.*/
                readyProducts.emplace_back(productName, std::move(product));

            } catch (BadProductException &e) {
                removeFromMenu(productName);
                /* W przypadku awarii maszyny, zaznaczenie informacji. */
                orderPtr->setStatus(Order::BROKEN_MACHINE);
                infoUpdater.updateFailedProduct(productName);
            }
        }

        if (orderPtr->getStatus() == Order::BROKEN_MACHINE) {
            infoUpdater.updateOrder(*orderPtr, WorkerReportUpdater::FAIL);
            /* Zwrócenie niewykorzystanych produktów do odpowiednich maszyn. */
            orderPtr->returnProducts(machines);
        } else {
            orderPtr->markDone(readyProducts);
            orderPtr->notifyClient();

            orderPtr->waitForClient(infoUpdater, machines);
        }

    }

    workerReports.push_back(infoUpdater.getReport());
}


