# Duże zadanie zaliczeniowe 3 - Cyrk

Pewien wesoły Klaun jest właścicielem sieci restauracji.
Ze względu na trudną sytuację na rynku IT postanowił też wziąć udział w zabawie i zwolnił wszystkich programistów.
Nie przewidział jednak, że zostawią system do obsługi zamówień w opłakanym stanie.

Prosi więc studentów o nieodpłatne stworzenie dla niego nowego oprogramowania zajmującego się obsługą zamówień,
pasującego do dostępnych urządzeń i do którego przyzwyczaili się inni pracownicy.

## Opis działania restauracji
Oto jak wygląda dzień działania pojedynczej restauracji.

W restauracji są dostępne maszyny wytwarzające produkty.
Na początku dnia uruchamiany jest system, którego częścią są maszyny oraz pewna liczba pracowników.

Do restauracji przychodzą klienci, którzy, aby dokonać zamówienia, muszą skorzystać z systemu.
Mogą się z niego dowiedzieć o aktualnie dostępnym menu i identyfikatorach złożonych zamówień oczekujących na realizację.
Za pośrednictwem systemu mogą też złożyć zamówienie i otrzymać urządzenie powiadamiające o gotowości zamówienia do odbioru.
Następnie, gdy zamówienie jest przygotowane, mogą, oddając urządzenie do systemu, odebrać gotowe produkty.

Pod koniec dnia system jest zamykany i zbierane są raporty pracowników z przebiegu ich pracy.

Dodatkowo w trakcie dnia, jak to w restauracjach Klauna bywa, dowolna maszyna może ulec awarii.
W przypadku takiej sytuacji, klienci, których zamówienie nie będzie mogło zostać zrealizowane, otrzymują stosowną informację
za pomocą urządzenia powiadamiającego, a produkt, którego nie można już wyprodukować, znika z menu.
Ponieważ osoby z uprawnieniami do naprawy maszyn pracują tylko nocą, danego dnia nikt już nie będzie próbował korzystać z zepsutej maszyny.

Zdarza się też, że klienci z różnych powodów nie odbierają swoich zamówień.
Jako że Klaun stara się być oszczędny, to jeżeli klient nie odbierze gotowego zamówienia przez czas określony na początku dnia,
to produkty z zamówienia mogą być wydane innym klientom. Po upłynięciu tego czasu klient bezpowrotnie traci możliwość
odbioru swojego zamówienia.

## Polecenie
Twoim zadaniem jest implementacja klas `System` oraz `CoasterPager` spełniającego interfejs dostarczony w pliku `system.hpp`.

Produkty są instancjami podklas klasy `Product`.

Zamówieniem nazywać będziemy multizbiór nazw produktów.
Zamówienie nazywamy zrealizowanym, kiedy wszystkie potrzebne produkty zostaną zebrane przez pracownika.

Maszyny są instancjami podklas klasy Machine. Klasa `Machine` implementuje metody:

- `getProduct` -- zwracająca `unique_ptr` na `Produkt`. Potencjalnie blokująca na czas wytworzenia produktu. Jeżeli maszyna uległa awarii podnosi wyjątek `MachineFailure`.
- `returnProduct` -- pozwalającą na zwrócenie produktu odpowiedniego typu do maszyny, np. w przypadku nieodebrania go przez klienta. Przy próbie zwrócenia produktu złego typu podnoszony jest wyjątek `BadProductException`. To czy maszyna uległa awarii, nie ma wpływu na działanie tej metody.
- `start` -- uruchamia maszynę, należy ją wykonać przed pierwszym użyciem maszyny.
- `stop` -- zatrzymuje maszynę (nie ma potrzeby wołania w przypadku awarii maszyny).

Metody klasy `Machine` nie gwarantują bezpieczeństwa.
Nie ma dwóch maszyn produkujących ten sam typ produktu.
Raz zepsuta maszyna, zawsze będzie już podnosić wyjątek przy wołaniu `getProduct.

Raport pracownika jest strukturą zawierającą następujące pola:

- `collectedOrders` -- wektor zrealizowanych zamówień
- `abandonedOrders` -- wektor zamówień, które nie zostały odebrane przez klienta
- `failedOrders` -- wektor zamówień, które nie mogły zostać zrealizowane z powodu awarii maszyny
- `failedProducts` -- wektor nazw produktów, na które oczekiwał, a które nie mogły zostać wyprodukowane z powodu awarii maszyny.


Urządzenia powiadamiające są instancjami klasy `CoasterPager`.
`CoasterPager` udostępnia 4 metody:

- `wait()` -- która blokuje, do czasu aż zamówienie będzie gotowe, a w przypadku awarii podnosi `FulfillmentFailure`.
- `wait(timeout)` -- j.w. ale blokuje maksymalnie na `timeout ms`.
- `getId` -- zwracająca unikatowy numer zamówienia. Numery zamówień powinny odpowiadać kolejności ich złożenia, t.j. jeżeli klient złożył zamówienie `A`, a potem zamówienie `B`, to `A` powinno mieć mniejszy numer niż `B`.
- `isReady` -- informująca czy zamówienie jest gotowe.

Za obsługę całego systemu odpowiada instancja klasy `System`.
Posiada ona następujące metody:

- `konstruktor` -- przyjmujący specyficzne dla danego dnia mapowanie nazw produktów dostępnych w menu na wskaźniki do maszyn w restauracji, liczbę pracowników oraz `clientTimeout`, czyli czas, po jakim gotowe, nieodebrane zamówienie może już nie zostać wydane.
- `shutdown` -- zamykająca system. Klienci nie mogą już złożyć nowych zamówień. Czeka na wydanie przygotowywanych zamówień, zbiera raporty pracowników i zwraca je w postaci wektora. Po zamknięciu systemu maszyny powinny być zatrzymane.
- `getMenu` -- zwracająca wektor nazw produktów dostępnych w menu. Po zamknięciu systemu zwraca pusty wektor.
- `getPendingOrders` -- zwracająca wektor numerów zamówień, które, nie zostały jeszcze odebrane przez klienta, nie upłynął dla nich czas oczekiwania ani nie uległa awarii żadna maszyna, na którą oczekiwał realizujący je pracownik.
- `order` -- przyjmująca wektor nazw produktów zamawianych przez klienta i zwracająca `unique_ptr pager`, który klient może użyć do odbioru zamówienia. W przypadku gdy została już wywołana metoda `shutdown`, podniesiony jest wyjątek `RestaurantClosedException`, natomiast gdy klient zamawia produkt, który nie jest dostępny w menu, podniesiony jest wyjątek `BadOrderException`.
- `collectOrder` -- przyjmująca `unique_ptr` na `pager` i zwracająca wektor produktów zamówionych przez klienta. Podnosi ona następujące wyjątki: 

	1.`FulfillmentFailure` -- w przypadku gdy jedna z potrzebnych do realizacji zamówienia maszyn zepsuła się przed odebraniem produktu przez pracownika realizującego zamówienie.
	2.`OrderNotReadyException` -- w przypadku gdy zamówienie nie jest jeszcze gotowe (ale może jeszcze zostać zrealizowane).
	3.`OrderExpiredException` -- w przypadku gdy zamówienie nie zostało odebrane przez klienta w czasie `clientTimeout`.
	4.`BadPagerException` -- w każdym innym przypadku gdy zamówienie nie może zostać przekazane klientowi (np. ktoś odebrał już nasze zamówienie, numer zamówienia nigdy nie wystąpił w systemie).

- `getClientTimeout` -- zwracająca czas, po jakim gotowe, nieodebrane zamówienie może już nie zostać wydane.

W przypadku wektorów zwracanych przez powyższe metody kolejność elementów nie jest istotna.

## Dodatkowe wymagania
System ma umożliwiać równoległą realizację `numberOfWorkers` zamówień, poprzez reprezentowanie pracowników w osobnych wątkach.

Pracownicy oczekują na zamówienia, a następnie je realizują poprzez zbieranie produktów z maszyn.
Wystarczy, że każdy pracownik będzie realizował tylko jedno zamówienie w danym momencie.
Gdy pracownik skompletuje zamówienie, powinien zaczekać na klienta, odebrać i zweryfikować `pager`, po czym wydać zamówienie i powrócić do oczekiwania na kolejne zamówienia.
Gdy podczas oczekiwania na produkt, maszyna zepsuje się, to pracownik jest zobowiązany poinformować o tym zajściu klienta, za pomocą urządzenia, zwrócić wszystkie zebrane produkty do odpowiednich maszyn i powrócić do oczekiwania na kolejne zamówienia.
Jeżeli klient nie odbierze zamówienia w czasie `clientTimeout`, pracownik także powinien zwrócić do maszyn wszystkie zgromadzone produkty.

Ponieważ klienci są wrażliwi na bycie ignorowanymi, zamówienia powinny być obsługiwane w kolejności ich składania, tzn. jeżeli zostały złożone dwa zamówienia i pierwsze z nich (złożone wcześniej) jest podzbiorem drugiego (złożonego później), to produkty każdego typu, z części wspólnej obu zamówień, zwrócone przez maszyny wcześniej, powinny trafić do pierwszego zamówienia.
Za zamówienie złożone wcześniej uznajemy to z mniejszym numerem.

### Przykład:
Treści zamówień:
```
    Zamówienie_1: Frytki, Frytki, Sok
    Zamówienie_2: Frytki, Sok, Burger
```
Maszyny zwracają kolejno (numeracja pomocnicza, mniejszy numer oznacza wcześniejsze zwrócenie produktu):
```
    Maszyna_Frytki: Frytki_1, Frytki_2, ...
    Maszyna_Sok: Sok_1, Sok_2, ...
    Maszyna_Burger: Burger_1, Burger_2...
```
Odbierając zamówienia, klienci otrzymują odpowiednio:
```
    Zamówienie_1: Frytki_1, Frytki_2, Sok_1
    Zamówienie_2: Frytki_3, Sok_2, Burger_1
```

Dodatkowo Klaun, ze względu na sposób działania maszyn, wymaga, aby, jeżeli jakiś pracownik potrzebuje danego produktu, to gdy produkt ten może zostać odebrany z maszyny (`getProduct` nie zablokuje) to produkt ten musi zostać odebrany natychmiast.
(Oznacza to, że pracownik powinien oczekiwać jednocześnie na wszystkie produkty, które są mu potrzebne do zamówienia).
Z tego samego powodu pracownik może odbierać tylko produkty, które są mu potrzebne do aktualnie realizowanego zamówienia.

Co prawda, nikt nie odważyłby się podrabiać pagera z restauracji Klauna, dlatego mamy pewność, że `pagery`, które system otrzymuje w `collectOrder` zostały wydane przez restaurację. Jednakże, próby naciągania i drobne oszustwa to zjawisko powszechne, więc należy uchronić restaurację przed stratami wydając zamówienie, tylko pierwszemu klientowi, który zgłosi się po odbiór zamówienia z danym numerem.

## Format oddawania
Program powinien być napisany w języku `C++` i kompilować się na students z flagami `-std=c++20 -Wall -Wextra -Werror -pedantic -fsanitize=undefined,thread`. Rozwiązania niespełniające tego warunku nie będą sprawdzane.
Rozwiązanie powinno składać się z jednego archiwum `ab123456.zip`, gdzie `ab123456` to login na `students`
i zawierać pliki: `system.hpp` i `system.cpp`.
Plik `system.hpp` można dowolnie modyfikować, nie zmieniając jednak publicznych interfejsów klas i struktur.

## Pytania
Pytania prosimy zadawać na forum.
[demo.cpp]()
[demo.cpp]()
[system.cpp]()
[system.hpp]()

