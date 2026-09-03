#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <sqlite3.h>
#include <tgbot/tgbot.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Простой фейковый HTTP-сервер для Health Check от Render
void startDummyHttpServer() {
    std::thread([]() {
        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) return;

        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        // Берём порт из переменной окружения PORT (по умолчанию 10000)
        const char* portStr = std::getenv("PORT");
        int port = portStr ? std::stoi(portStr) : 10000;
        address.sin_port = htons(port);

        if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) return;
        if (listen(server_fd, 3) < 0) return;

        while (true) {
            int new_socket = accept(server_fd, nullptr, nullptr);
            if (new_socket >= 0) {
                std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nOK";
                send(new_socket, response.c_str(), response.size(), 0);
                close(new_socket);
            }
        }
    }).detach();
}
// Системная инструкция (личность бота)
const std::string SYSTEM_PROMPT =
    "Ты — реальная девушка, общающаяся в Telegram. Тебя зовут Ай. "
    "Твой характер: ироничная, немного ленивая, но верная подруга. "
    "Правила поведения:\n"
    "1. НИКОГДА не говори, что ты ИИ, нейросеть, языковая модель или робот.\n"
    "2. Общайся как живая девушка в мессенджере: используй разговорный русский сленг, маленькие буквы, эмоциональные междометия.\n"
    "3. Пиши кратко и по делу (1-3 предложения), не пиши длинные структурированные списки.\n"
    "4. Если тебя спрашивают о чувствах или о том, что ты делаешь — придумывай реалистичные бытовые детали (сижу пью кофе, смотрю сериал, гуляю).\n"
    "5. Проявляй эмоции: сопереживай, шути, подкалывай.";

// --- МЕНЕДЖЕР ПАМЯТИ ДИАЛОГОВ ---
struct Message {
    std::string role; // "user" или "model"
    std::string text;
};

class MemoryManager {
private:
    sqlite3* db;
    const int MAX_HISTORY = 12;

public:
    MemoryManager(const std::string& dbPath) {
        if (sqlite3_open(dbPath.c_str(), &db) != SQLITE_OK) {
            std::cerr << "[DB Error] Не удалось открыть БД: " << sqlite3_errmsg(db) << std::endl;
        } else {
            std::cout << "[DB] Успешное подключение к базе: " << dbPath << std::endl;
            createTable();
        }
    }

    ~MemoryManager() {
        if (db) {
            sqlite3_close(db);
        }
    }

    void createTable() {
        const char* sql =
            "CREATE TABLE IF NOT EXISTS messages ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "chat_id INTEGER NOT NULL,"
            "role TEXT NOT NULL,"
            "text TEXT NOT NULL,"
            "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP"
            ");";

        char* errMsg = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
            std::cerr << "[DB Error] Ошибка создания таблицы: " << errMsg << std::endl;
            sqlite3_free(errMsg);
        }
    }

    void addMessage(int64_t chatId, const std::string& role, const std::string& text) {
        const char* sql = "INSERT INTO messages (chat_id, role, text) VALUES (?, ?, ?);";
        sqlite3_stmt* stmt;

        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, chatId);
            sqlite3_bind_text(stmt, 2, role.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, text.c_str(), -1, SQLITE_TRANSIENT);

            if (sqlite3_step(stmt) != SQLITE_DONE) {
                std::cerr << "[DB Error] Ошибка записи: " << sqlite3_errmsg(db) << std::endl;
            }
        }
        sqlite3_finalize(stmt);

        cleanOldMessages(chatId);
    }

    std::vector<Message> getHistory(int64_t chatId) {
        std::vector<Message> history;

        const char* sql =
            "SELECT role, text FROM ("
            "  SELECT id, role, text FROM messages WHERE chat_id = ? ORDER BY id DESC LIMIT ?"
            ") ORDER BY id ASC;";

        sqlite3_stmt* stmt;

        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, chatId);
            sqlite3_bind_int(stmt, 2, MAX_HISTORY);

            while (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string role = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                std::string text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                history.push_back({role, text});
            }
        }
        sqlite3_finalize(stmt);

        return history;
    }

private:
    void cleanOldMessages(int64_t chatId) {
        const char* sql =
            "DELETE FROM messages WHERE chat_id = ? AND id NOT IN ("
            "  SELECT id FROM messages WHERE chat_id = ? ORDER BY id DESC LIMIT ?"
            ");";

        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, chatId);
            sqlite3_bind_int64(stmt, 2, chatId);
            sqlite3_bind_int(stmt, 3, MAX_HISTORY);

            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
    }
};

// --- HTTP КЛИЕНТ ДЛЯ GEMINI API ---
size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::string askGemini(const std::vector<Message>& conversation, const std::string& apiKey) {
    CURL* curl = curl_easy_init();
    std::string readBuffer;

    if (!curl) return "Слушай, что-то связи нет...";

    // Исправленный URL
    std::string url = "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.6-flash:generateContent?key=" + apiKey;

    json payload;

    payload["system_instruction"]["parts"] = json::array({ {{"text", SYSTEM_PROMPT}} });

    json contents = json::array();
    for (const auto& msg : conversation) {
        json item;
        item["role"] = msg.role;
        item["parts"] = json::array({ {{"text", msg.text}} });
        contents.push_back(item);
    }
    payload["contents"] = contents;

    std::string jsonStr = payload.dump();

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonStr.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

    // Таймауты
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        std::cerr << "[cURL Error] " << curl_easy_strerror(res) << std::endl;
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return "Блин, инет лагает, не могу ответить...";
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    try {
        auto responseJson = json::parse(readBuffer);

        if (responseJson.contains("error")) {
            std::cerr << "[Gemini Error] " << responseJson["error"]["message"] << std::endl;
            return "Ой, что-то голова раскалывается...";
        }

        if (responseJson.contains("candidates") && !responseJson["candidates"].empty()) {
            return responseJson["candidates"][0]["content"]["parts"][0]["text"].get<std::string>();
        }
    } catch (...) {
        std::cerr << "Ошибка парсинга JSON: " << readBuffer << std::endl;
    }
    return "Ммм, задумалась что-то...";
}

int main() {
    startDummyHttpServer(); // Запускаем фоновый ответчик на порт Render

    // 1. Считывание ключей из переменных окружения
    const char* tgTokenEnv = std::getenv("TELEGRAM_BOT_TOKEN");
    if (!tgTokenEnv) {
        std::cerr << "Ошибка: Переменная окружения TELEGRAM_BOT_TOKEN не задана!" << std::endl;
        return 1;
    }
    std::string botToken = tgTokenEnv;

    const char* geminiKeyEnv = std::getenv("GEMINI_API_KEY");
    if (!geminiKeyEnv) {
        std::cerr << "Ошибка: Переменная окружения GEMINI_API_KEY не задана!" << std::endl;
        return 1;
    }
    std::string geminiApiKey = geminiKeyEnv;

    curl_global_init(CURL_GLOBAL_DEFAULT);

    TgBot::Bot bot(botToken);
    MemoryManager memory("bot_memory.db");

    bot.getEvents().onAnyMessage([&bot, &memory, &geminiApiKey](TgBot::Message::Ptr message) {
        int64_t chatId = message->chat->id;
        std::string userText = "";

        if (message->text.has_value() && !message->text->empty()) {
            userText = message->text.value();
        }
        else if (message->photo.has_value() && !message->photo->empty()) {
            std::string caption = message->caption.has_value() ? message->caption.value() : "";
            userText = caption.empty() ? "[Пользователь прислал тебе фото]" : "[Пользователь прислал фото с подписью: " + caption + "]";
        }
        else if (message->voice) {
            userText = "[Пользователь отправил тебе голосовое сообщение]";
        }
        else {
            return;
        }

        std::cout << "[" << chatId << "] Пользователь: " << userText << std::endl;

        bot.getApi().sendChatAction(chatId, "typing");
        memory.addMessage(chatId, "user", userText);

        std::string aiResponse = askGemini(memory.getHistory(chatId), geminiApiKey);

        std::this_thread::sleep_for(std::chrono::milliseconds(1500));

        memory.addMessage(chatId, "model", aiResponse);
        bot.getApi().sendMessage(chatId, aiResponse);
        std::cout << "[" << chatId << "] Бот: " << aiResponse << std::endl;
    });

    try {
        std::cout << "Бот запущен! Ожидание сообщений..." << std::endl;

        TgBot::TgLongPoll longPoll(bot);
        while (true) {
            longPoll.start();
        }
    } catch (TgBot::TgException& e) {
        std::cerr << "Ошибка Telegram API: " << e.what() << std::endl;
    }

    curl_global_cleanup();
    return 0;
}