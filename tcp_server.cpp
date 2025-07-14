// TCP Calculator Server (server.cpp)
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <stack>
#include <string>
#include <unordered_map>
#include <vector>

constexpr int MAX_EVENTS = 1000; // Максимальное количество событий для epoll

// Устанавливает неблокирующий режим для файлового дескриптора
int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Возвращает приоритет оператора
int precedence(char op) {
    if (op == '+' || op == '-') return 1;
    if (op == '*' || op == '/') return 2;
    return 0;
}

// Применяет оператор op к значениям a и b
long apply_op(long a, long b, char op) {
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/':
            if (b == 0) throw std::runtime_error("Division by zero");
            return a / b;
    }
    throw std::runtime_error("Unknown operator");
}

// Функция вычисления целочисленного выражения с учётом приоритета операций
long evaluate(const std::string& s) {
    std::stack<long> values;  // стек для чисел
    std::stack<char> ops;     // стек для операторов

    for (size_t i = 0; i < s.size();) {
        if (std::isspace(static_cast<unsigned char>(s[i]))) {
            ++i;
        }
        else if (std::isdigit(static_cast<unsigned char>(s[i]))) {
            // читаем целое число
            long val = 0;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
                val = val * 10 + (s[i++] - '0');
            }
            values.push(val);
        }
        else {
            // текущий символ — оператор
            char op = s[i++];
            // пока на вершине стека ops есть оператор с приоритетом >= текущего
            while (!ops.empty() && precedence(ops.top()) >= precedence(op)) {
                long b = values.top(); values.pop();
                long a = values.top(); values.pop();
                char top_op = ops.top(); ops.pop();
                values.push(apply_op(a, b, top_op));
            }
            ops.push(op);
        }
    }

    // остающиеся операции
    while (!ops.empty()) {
        long b = values.top(); values.pop();
        long a = values.top(); values.pop();
        char top_op = ops.top(); ops.pop();
        values.push(apply_op(a, b, top_op));
    }

    if (values.empty()) throw std::runtime_error("Empty expression");
    return values.top();
}

// Структура для хранения буферов соединения
struct Connection {
    std::string in_buf;  // Буфер входящих данных
    std::string out_buf; // Буфер исходящих данных
};

int main(int argc, char* argv[]) {
    // Проверяем аргументы командной строки
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <port>\n";
        return 1;
    }
    int port = std::stoi(argv[1]); // Порт для прослушивания

    // Создаем слушающий сокет
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }
    set_nonblocking(listen_fd); // Делаем сокет неблокирующим

    int opt = 1;
    // Повторное использование адреса
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;       // Принимаем на всех интерфейсах
    addr.sin_port = htons(port);             // Преобразуем порт в сетевой порядок
    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }

    if (listen(listen_fd, SOMAXCONN) < 0) { perror("listen"); return 1; }

    // Создаем epoll-демон
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) { perror("epoll_create1"); return 1; }

    // Регистрируем слушающий дескриптор только на чтение
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

    // Хранилище подключений и событий
    std::unordered_map<int, Connection> conns;
    std::vector<epoll_event> events(MAX_EVENTS);

    std::cout << "Server listening on port " << port << std::endl;

    while (true) {
        int n = epoll_wait(epoll_fd, events.data(), MAX_EVENTS, -1);
        if (n < 0 && errno == EINTR) continue; // Повторить при прерывании сигналом

        for (int i = 0; i < n; ++i) {
            int fd  = events[i].data.fd;
            uint32_t evs = events[i].events;

            if (fd == listen_fd) {
                // Обработка новых подключений
                while (true) {
                    sockaddr_in client;
                    socklen_t len = sizeof(client);
                    int conn_fd = accept(listen_fd, (sockaddr*)&client, &len);
                    if (conn_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept");
                        break;
                    }
                    set_nonblocking(conn_fd); // Неблокирующий режим
                    epoll_event client_ev{};
                    client_ev.events = EPOLLIN | EPOLLET;
                    client_ev.data.fd = conn_fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_fd, &client_ev);
                    conns[conn_fd] = Connection{};
                    std::cout << "Accepted connection fd=" << conn_fd << std::endl;
                }
            }
            else {
                auto &c = conns[fd];

                // Чтение данных от клиента
                if (evs & EPOLLIN) {
                    char buf[512];
                    while (true) {
                        ssize_t count = read(fd, buf, sizeof(buf));
                        if (count > 0) {
                            c.in_buf.append(buf, count);
                        }
                        else if (count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                            break; // Прочитали всё
                        }
                        else {
                            // Клиент закрыл или произошла ошибка
                            close(fd);
                            conns.erase(fd);
                            goto next_event;
                        }
                    }
                    // Обработка завершённых выражений (разделитель — пробел)
                    size_t pos;
                    while ((pos = c.in_buf.find(' ')) != std::string::npos) {
                        std::string expr = c.in_buf.substr(0, pos);
                        c.in_buf.erase(0, pos + 1);

                        std::string reply;
                        try {
                            long res = evaluate(expr);
                            reply = std::to_string(res);
                        } catch (...) {
                            reply = "ERR";
                        }
                        reply.push_back(' ');
                        c.out_buf += reply;
                        std::cout << "Expr: '" << expr << "' -> " << reply << std::endl;

                        // Включаем EPOLLOUT для отправки
                        epoll_event mod{};
                        mod.events = EPOLLIN | EPOLLOUT | EPOLLET;
                        mod.data.fd = fd;
                        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &mod);
                    }
                }

                // Отправка ответов клиенту
                if (evs & EPOLLOUT) {
                    while (!c.out_buf.empty()) {
                        ssize_t written = write(fd, c.out_buf.data(), c.out_buf.size());
                        if (written > 0) {
                            c.out_buf.erase(0, written);
                        }
                        else if (written == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                            break; // Нечем больше писать
                        }
                        else {
                            close(fd);
                            conns.erase(fd);
                            goto next_event;
                        }
                    }
                    // Если буфер пуст, выключаем EPOLLOUT
                    if (c.out_buf.empty()) {
                        epoll_event mod{};
                        mod.events = EPOLLIN | EPOLLET;
                        mod.data.fd = fd;
                        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &mod);
                    }
                }
            }

        next_event:;
        }
    }

    close(listen_fd);
    return 0;
}
