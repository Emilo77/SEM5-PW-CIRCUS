#include <algorithm>
#include "system.hpp"


void Order::waitForOrder() {
    std::unique_lock<std::mutex> lock(*orderInfoMutex);
    clientNotifier->wait(lock, [this] { return status != NOT_DONE; });

    if (status != READY) {
        throw FulfillmentFailure();
    }
}

void Order::waitForOrderTimed(unsigned int time) {
    std::unique_lock<std::mutex> lock(*orderInfoMutex);
    clientNotifier->wait_for(lock, std::chrono::milliseconds(time), [this] {
        return status != NOT_DONE;
    });
    if (status != READY) {
        throw FulfillmentFailure();
    }
}

void Order::waitForClient(WorkerReportUpdater &infoUpdater,
                          Order::machine_wrappers_t &machines) {
    std::unique_lock<std::mutex> lock(*orderInfoMutex);

    auto clientCollected = workerNotifier->wait_for(lock,
                                                    std::chrono::milliseconds(
                                                            timeout), [this] {
                return readyProducts.empty();
            });

    if (clientCollected) {
        /* Klient pojawił się na czas i odebrał produkty. */
        setStatus(Order::RECEIVED);
        infoUpdater.updateReport(*this, WorkerReportUpdater::COLLECT);
    } else {
        /* Jeżeli klient nie pojawił się na czas, zmienienie statusu
                 * zamówienia i zwrócenie produktów do maszyn. */
        setStatus(Order::EXPIRED);
        infoUpdater.updateReport(*this, WorkerReportUpdater::ABANDON);
        returnProducts(machines);
    }
}

void
Order::markOrderDone(
        std::vector<std::pair<std::string, std::unique_ptr<Product>>>
        &namedProducts) {
    /* Dodanie produktów do zamówienia, zaktualizowanie statusu. */
    readyProducts = std::move(namedProducts);
    doneTime = std::chrono::system_clock::now();
    status = READY;
}

std::vector<std::unique_ptr<Product>> Order::collectReadyProducts() {
    /* Zakładamy, że funkcja wywoływana jest po locku na ochronę klasy. */
    /* Wyciągnięcie gotowych produktów, ich nazwy są już niepotrzebne. */
    std::vector<std::unique_ptr<Product>> achievedProducts;
    for (auto &namedProduct: readyProducts) {
        achievedProducts.push_back(std::move(namedProduct.second));
    }
    setStatus(RECEIVED);
    /* Wyczyszczenie kontenera na produkty. */
    readyProducts.clear();

    /* Zwrócenie gotowych produktów. */
    return achievedProducts;
}

void Order::returnProducts(machine_wrappers_t &machines) {
    for (auto &product: readyProducts) {
        machines.at(product.first)->returnProduct(product.second);
    }
    readyProducts.clear();
}

std::vector<std::string> Order::getProductNames() {
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
    return status;
}

void Order::setStatusLocked(OrderStatus newStatus) {
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

std::vector<std::unique_ptr<Product>>
Order::tryToCollectOrder() {
    std::unique_lock<std::mutex> lock(*orderInfoMutex);
    updateIfExpired();

    switch (status) {
        case Order::BROKEN_MACHINE:
            throw FulfillmentFailure();
        case Order::NOT_DONE:
            throw OrderNotReadyException();
        case Order::EXPIRED:
            throw OrderExpiredException();
        case Order::RECEIVED:
            throw BadPagerException();
        case Order::READY: {
            /* Zwrócenie produktów klientowi. */
            auto result = collectReadyProducts();
            lock.unlock();
            notifyWorker();
            return result;
        }
    }
    throw BadPagerException();
}

void WorkerReportUpdater::updateReport(Order &order, ACTION a) {
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


void OrderQueue::pushOrder(std::shared_ptr<Order> order) {
    {
        std::unique_lock<std::mutex> lock(queMutex);
        que.emplace(std::move(order));
    }
    queCondition.notify_one();
}

std::optional<std::shared_ptr<Order>>
OrderQueue::popOrder(std::unique_lock<std::mutex> &lock, std::atomic_bool
&systemOpen) {
    /* Zakładamy, że lock zostanie ustawiony przed wywołaniem funkcji. */
    queCondition.wait(lock, [this, &systemOpen] {
        return (!que.empty() || !systemOpen);
    });
    /* Jeżeli kolejka jest pusta i pracownik został obudzony, oznacza to, że
     * nie ma już więcej pracy do wykonania. */
    if (que.empty()) {
        return {};
    }
    std::shared_ptr<Order> order(std::move(que.front()));
    que.pop();

    return order;
}

bool OrderQueue::isEmpty() {
    std::unique_lock<std::mutex> lock(queMutex);
    return que.empty();
}


void MachineQueue::pushPromise(promise_ptr value) {
    {
        std::unique_lock<std::mutex> lock(queMutex);
        que.emplace(std::move(value));
    }
    queCondition.notify_one();
}


std::optional<promise_ptr> MachineQueue::popPromise(std::atomic_bool
                                                          &orderWorkersEnded) {
    std::unique_lock<std::mutex> lock(queMutex);
    queCondition.wait(lock, [this, &orderWorkersEnded] {
        return (!que.empty() || orderWorkersEnded);
    });
    if (orderWorkersEnded && que.empty()) {
        return {};
    }
    auto promiseProduct = que.front();
    que.pop();

    return promiseProduct;
}

void MachineWrapper::machineWorker() {
    /* Obsługa maszyny trwa do momentu, aż wszystkie wątki kompletujące
     * zamówienia zakończą pracę. */
    while (!orderWorkersEnded) {
        /* Czekamy na kolejce, wyciągamy polecenie wyprodukowania
         * produktu. */

        /* Wyciągnięcie promise'u z kolejki blokującej. */
        auto promiseProductOption = machineQueue.popPromise(orderWorkersEnded);

        /* Sytuacja, w której wyszliśmy z czekania z kolejki, bo wszyscy
         * pracownicy od zamówień skończyli pracę i nie ma już żadnej
         * pracy do wykonania. */
        if (!promiseProductOption.has_value() ||
                (promiseProductOption.value() == nullptr)) {
            break;
        }

        if (status == OFF) {
            startMachine();
        }

        auto promiseProduct = promiseProductOption.value();

        try {
            auto p = machine->getProduct();
            promiseProduct->set_value(std::move(p));

        } catch (MachineFailure &e) {
            promiseProduct->set_exception(std::current_exception());
        }
    }

    if (status == ON) {
        stopMachine();
    }
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

    /* Przechowywanie maszyn we wrapperach. */
    for (auto &machine: machines) {
        this->machines.emplace(machine.first,
                               std::make_shared<MachineWrapper>(
                                       machine.first,
                                       machine.second,
                                       orderWorkersEnded));
    }

    /* Dodanie menu. */
    for (auto &machine: machines) {
        menu.emplace(machine.first);
    }

    for (unsigned int i = 0; i < numberOfWorkers; i++) {
        orderWorkers.emplace_back([this]() { orderWorker(); });
    }
}


std::vector<WorkerReport> System::shutdown() {
    {
        std::unique_lock<std::mutex> orderQueueLock(orderQueue.getMutex());
        if(!systemOpen) {
            return workerReports;
        }
        systemOpen = false;
    }

    orderQueue.notifyShutdown();

    for (auto &w: orderWorkers) {
        if (w.joinable()) {
            w.join();
        }
    }


    orderWorkersEnded = true;
    for (auto &machine: machines) {
        machine.second->notifyShutdown();
    }


    for (auto &element: machines) {
        element.second->joinMachineWorker();
    }

    return workerReports;
}

std::vector<std::string> System::getMenu() const {
    std::vector<std::string> menuVector;
    {
        std::unique_lock<std::mutex> lock(menuMutex);
        if (systemOpen && !orderWorkers.empty()) {
            menuVector = std::vector<std::string>(menu.begin(), menu.end());
        }
    }
    return menuVector;
}


std::vector<unsigned int> System::getPendingOrders() const {
    std::vector<unsigned int> pendingOrders;

    {
        std::unique_lock<std::mutex> lock(ordersMapMutex);
        for (auto &element: orders) {
            if (element.second->checkIfPending()) {
                pendingOrders.push_back(element.second->getId());
            }
        }
    }

    return pendingOrders;
}


std::unique_ptr<CoasterPager> System::order(std::vector<std::string> products) {
    std::unique_lock<std::mutex> newOrderLock(newOrderMutex);

    if (!systemOpen || orderWorkers.empty()) {
        throw RestaurantClosedException();
    }

    if (products.empty() || !productsInMenu(products)) {
        throw BadOrderException();
    }

    size_t newId = idGenerator.newId();
    auto newOrder = std::make_shared<Order>(
            Order(newId, products, clientTimeout));
    {
        std::unique_lock<std::mutex> ordersLock(ordersMapMutex);
        orders.emplace(newId, newOrder);
    }
    orderQueue.pushOrder(newOrder);

    return std::make_unique<CoasterPager>(CoasterPager(newOrder));
}


std::vector<std::unique_ptr<Product>>
System::collectOrder(std::unique_ptr<CoasterPager> pager) {
    if (pager == nullptr) {
        throw BadPagerException();
    }

    try {
        return pager->order->tryToCollectOrder();
    } catch (...) {
        throw;
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
        auto orderOption = orderQueue.popOrder(lock, systemOpen);

        /* Sytuacja zamknięcia systemu. */
        if (!orderOption.has_value()) {
            lock.unlock();
            break;
        }

        auto order = orderOption->get();

        /* Zlecenie maszynom wyprodukowanie produktów. */
        std::vector<std::pair<std::string, product_future_t>> futureProducts;

        {
            std::unique_lock<std::mutex> orderMutex(*(order->getInfoMutex()));

            for (const auto &productName: order->getProductNames()) {
                auto newPromise = std::make_shared<product_promise_t>();

                futureProducts.emplace_back(productName, newPromise->get_future());
                machines.at(productName)->insertToQueue(std::move(newPromise));
            }
        }

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

            } catch (MachineFailure &e) {
                removeFromMenu(productName);
                /* W przypadku awarii maszyny, zaznaczenie informacji. */
                order->setStatusLocked(Order::BROKEN_MACHINE);
                order->notifyClient();
                infoUpdater.updateReport(productName);
            }
        }

        std::unique_lock<std::mutex> orderLock(*(order->getInfoMutex()));

        if (order->getStatus() == Order::BROKEN_MACHINE) {
            infoUpdater.updateReport(*order, WorkerReportUpdater::FAIL);
            /* Zwrócenie niewykorzystanych produktów do odpowiednich maszyn. */
            order->returnProducts(machines);
            orderLock.unlock();
            order->notifyClient();

        } else {
            order->markOrderDone(readyProducts);

            orderLock.unlock();
            /* Powiadomienie klienta czekającego na funkcjach z pager'a. */
            order->notifyClient();

            order->waitForClient(infoUpdater, machines);
        }
    }

    {
        std::unique_lock<std::mutex> workerReportsLock(workerReportsMutex);
        infoUpdater.addReport(workerReports);
    }
}


