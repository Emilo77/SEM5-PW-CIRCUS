#include <algorithm>
#include "system.hpp"

void print2(std::string s) {
    std::cout << std::this_thread::get_id() << ": " << s << std::endl;
}

typedef std::promise<std::unique_ptr<Product>> product_promise_t;
typedef std::future<std::unique_ptr<Product>> product_future_t;

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
        case Order::OTHER:
            throw BadPagerException();
        case Order::READY: {
            if (readyProducts.empty()) {
                exit(420);
            } else {
                /* Zwrócenie produktów klientowi. */
                auto result = collectReadyProducts();
                lock.unlock();
                notifyWorker();
                return result;
            }
        }
    }
    throw BadPagerException();
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
    actionMade = true;
}


void OrderQueue::pushOrder(std::shared_ptr<Order> order) {
    {
        std::unique_lock<std::mutex> lock(d_mutex);
        d_queue.push_front(order);
    }
    d_condition.notify_one();
}

std::shared_ptr<Order>
OrderQueue::popOrder(std::unique_lock<std::mutex> &lock, std::atomic_bool
&systemOpen) {
    /* Zakładamy, że lock zostanie ustawiony przed wywołaniem funkcji. */
    d_condition.wait(lock, [this, &systemOpen] {
        return (!d_queue.empty() || !systemOpen);
    });
    if (d_queue.empty()) {
        return nullptr;
    }
    std::shared_ptr<Order> order(std::move(d_queue.back()));
    d_queue.pop_back();

    return order;
}

bool OrderQueue::isEmpty() {
    std::unique_lock<std::mutex> lock(d_mutex);
    return d_queue.empty();
}


void MachineQueue::pushPromise(product_promise_t value) {
    {
        std::unique_lock<std::mutex> lock(d_mutex);
        d_queue.push_back(std::move(value));
    }
    d_condition.notify_one();
}


std::optional<product_promise_t> MachineQueue::popPromise(std::atomic_bool
                                                          &orderWorkersEnded) {
    std::unique_lock<std::mutex> lock(d_mutex);
    d_condition.wait(lock, [this, &orderWorkersEnded] {
        return (!d_queue.empty() || orderWorkersEnded);
    });
    if (orderWorkersEnded && d_queue.empty()) {
        return {};
    }
    product_promise_t promiseProduct(std::move(d_queue.back()));
    d_queue.pop_front();

    return promiseProduct;
}

void MachineWrapper::machineWorker() {
    /* Obsługa maszyny trwa do momentu, aż wszystkie wątki kompletujące
     * zamówienia zakończą pracę. */
    while (!orderWorkersEnded) {
        /* Czekamy na kolejce, wyciągamy polecenie wyprodukowania
         * produktu. */

        auto promiseProductOption = machineQueue.popPromise(orderWorkersEnded);

        /* Sytuacja, w której wyszliśmy z czekania z kolejki, bo wszyscy
         * pracownicy od zamówień skończyli pracę i nie ma już żadnej
         * pracy do wykonania. */
        if (!promiseProductOption.has_value()) {
            break;
        }

        if (status == OFF) {
            startMachine();
        }

        try {
            auto product = machine->getProduct();
            cout << "Przed promisem set_value\n";
            promiseProductOption.value().set_value(std::move(product));
            cout << "Po promisie set_value\n";

        } catch (MachineFailure &e) {
            promiseProductOption.value().set_exception(std::current_exception());
        } catch (const std::future_error &e) {
            cout << "Promise set_value(): " << e.what() << "\n";
        }
    }

    if (status == ON) {
        stopMachine();
    }
}

std::vector<std::pair<std::string, product_future_t>>
OrderSynchronizer::insertOrderToMachines(Order &order,
                                         std::unordered_map<std::string,
                                                 std::shared_ptr<MachineWrapper>> &machines) {

    std::unique_lock<std::mutex> lock(*order.getMutex());

    std::vector<std::pair<std::string, product_future_t>> futureProducts;
    for (const auto &productName: order.getProductNames()) {
        product_promise_t newPromise;
        auto future = newPromise.get_future();
        futureProducts.emplace_back(productName, std::move(future));
        machines.at(productName)->insertToQueue(std::move(newPromise));
    }
    return futureProducts;
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
    for (auto machine: machines) {
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
    {
        std::unique_lock<std::mutex> orderQueueLock(orderQueue.getMutex());
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
        if (systemOpen) {
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

    if (!systemOpen) {
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
    std::unique_lock<std::mutex> lock(collectOrderMutex);
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
        auto orderPtr = orderQueue.popOrder(lock, systemOpen);

        /* Sytuacja zamknięcia systemu. */
        if (orderPtr == nullptr) {
            lock.unlock();
            break;
        }

        /* Zlecenie maszynom wyprodukowanie produktów. */
        auto futureProducts = orderSynchronizer.insertOrderToMachines(
                *orderPtr, machines);

        /* Odblokowanie kolejki zamówień. */
        lock.unlock();

        /* Wyprodukowane produkty przez maszyny. */
        std::vector<std::pair<std::string, std::unique_ptr<Product>>>
                readyProducts;

        for (auto &futureProduct: futureProducts) {
            std::string productName = futureProduct.first;
            try {
                cout << "Przed pobraniem wyniku get\n";
                auto product = futureProduct.second.get();
                cout << "Po pobraniu wyniku get\n";

                /* Jeżeli wyprodukowanie powiodło się, dodanie do kontenera.*/
                readyProducts.emplace_back(productName, std::move(product));

            } catch (MachineFailure &e) {
                removeFromMenu(productName);
                /* W przypadku awarii maszyny, zaznaczenie informacji. */
                orderPtr->setStatusLocked(Order::BROKEN_MACHINE);
                orderPtr->notifyClient();
                infoUpdater.updateFailedProduct(productName);
            } catch (std::future_error &e) {
                cout << "odbieranie future: " << e.what() << endl;
            }
        }

        std::unique_lock<std::mutex> orderLock(*(orderPtr->getMutex()));

        if (orderPtr->getStatus() == Order::BROKEN_MACHINE) {
            infoUpdater.updateOrder(*orderPtr, WorkerReportUpdater::FAIL);
            /* Zwrócenie niewykorzystanych produktów do odpowiednich maszyn. */
            orderPtr->returnProducts(machines);
            orderLock.unlock();
            orderPtr->notifyClient();

        } else {
            orderPtr->markOrderDone(readyProducts);
            orderLock.unlock();
            /* Powiadomienie klienta czekającego na funkcjach z pager'a. */
            orderPtr->notifyClient();

            orderPtr->waitForClient(infoUpdater, machines);
        }

    }

    infoUpdater.addReport(workerReports);
}


