/*! \file common.h
 *  \brief Enter description here.
 *  \author Georgi Gerganov
 */

#pragma once

#include <cstdlib>
#include <chrono>
#include <string>
#include <queue>
#include <thread>

// ;(( ...
namespace {
    inline float frand() { return (float)(rand())/RAND_MAX; }

    inline uint64_t timestamp_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    inline float timestamp_s() {
        static auto tStart = timestamp_ms();
        return 0.001*(timestamp_ms() - tStart);
    }

    // trim from start (in place)
    static inline void ltrim(std::string &s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](int ch) {
            return !std::isspace(ch);
        }));
    }

    // trim from end (in place)
    static inline void rtrim(std::string &s) {
        s.erase(std::find_if(s.rbegin(), s.rend(), [](int ch) {
            return !std::isspace(ch);
        }).base(), s.end());
    }

    // trim from both ends (in place)
    static inline void trim(std::string &s) {
        ltrim(s);
        rtrim(s);
    }
}

struct Client {
    std::string name = "__unknown__";
    std::string color = "#000000";
};

struct PlayerInfo {
    int32_t clientId = 0;

    float score = 0.0f;

    bool active = false;
};

struct Query {
    float x = 0.0f;
    float y = 0.0f;

    int32_t clientId = 0;
    float tGuessed_s = 0.0f;

    std::string text;
    std::string answer;
};

struct Event {
    enum Type {
        Unknown = 0,
        StartNewRound,
        EndRound,
        PlayerJoinRoom,
        PlayerInput,
        ClientChangeName,
        ClientChangeColor,
    };

    Type type = Unknown;

    int32_t roomId = -100;
    int32_t clientId = -1;

    std::string inputStr;
};

struct Events {
    std::deque<Event> data;

    std::mutex mutex;
    std::condition_variable cv;

    void push(Event && event) {
        std::lock_guard<std::mutex> lock(mutex);
        data.push_back(std::move(event));
        cv.notify_one();
    }
};

struct Notifier {
    std::thread th;

    Events * events = nullptr;

    void notify(std::function<void()> && f) {
        if (th.joinable()) th.join();
        th = std::thread(std::move(f));
    }
};

struct RoomBase {
    int32_t id = 0;

    std::string name = "__unknown__";

    int32_t roundId = 0;
    int32_t poolSize = 0;

    float tCreated_s = 0.0f;
    float tRoundStart_s = 0.0f;
    float tRoundLength_s = 0.0f;
    float tNextRoundStart_s = 0.0f;

    uint64_t tTimeBetweenRounds_ms = 20000;

    std::vector<Query> pool;
    std::vector<Query> poolOld;
    std::vector<PlayerInfo> players;

    Notifier notifier;

    virtual Query generate() = 0;

    virtual void handle(Event && event) {
        switch (event.type) {
            case Event::StartNewRound:
                {
                    if (event.roomId != id) return;

                    ++roundId;
                    printf("Room %d: starting new round %d\n", id, roundId);

                    pool.clear();
                    poolOld.clear();
                    while (pool.size() < poolSize) {
                        pool.emplace_back(generate());
                    }

                    players.erase(std::remove_if(players.begin(), players.end(),
                                                 [](const auto & p) { return p.active == false; }),
                                  players.end());

                    for (auto & player : players) {
                        player.score = 0.0f;
                    }

                    tRoundStart_s = timestamp_s();

                    notifier.notify([&]() {
                        std::this_thread::sleep_for(std::chrono::milliseconds((uint64_t)(1000.0*tRoundLength_s)));

                        Event event;
                        event.roomId = id;
                        event.type = Event::EndRound;

                        notifier.events->push(std::move(event));
                    });
                }
                break;
            case Event::EndRound:
                {
                    if (event.roomId != id) return;

                    printf("Room %d: ending round %d\n", id, roundId);

                    tNextRoundStart_s = timestamp_s() + 0.001f*tTimeBetweenRounds_ms;

                    notifier.notify([&]() {
                        std::this_thread::sleep_for(std::chrono::milliseconds(tTimeBetweenRounds_ms));

                        Event event;
                        event.roomId = id;
                        event.type = Event::StartNewRound;

                        notifier.events->push(std::move(event));
                    });
                }
                break;
            case Event::PlayerJoinRoom:
                {
                    if (event.roomId == id) {
                        for (auto & player : players) {
                            if (player.clientId == event.clientId) {
                                player.active = true;
                                return;
                            }
                        }

                        PlayerInfo player;

                        player.active = true;
                        player.clientId = event.clientId;

                        players.emplace_back(std::move(player));

                        printf("Client (%d) joined room '%s' (%d)\n", event.clientId, name.c_str(), id);
                    } else {
                        for (auto & player : players) {
                            if (player.clientId == event.clientId) {
                                player.active = false;

                                printf("Client (%d) left room '%s' (%d)\n", event.clientId, name.c_str(), id);
                            }
                        }
                    }
                }
                break;
            case Event::PlayerInput:
                {
                    if (event.roomId != id) return;
                    if (::timestamp_s() > tRoundStart_s + tRoundLength_s) return;

                    int playerId = -1;
                    for (int i = 0; i < (int) players.size(); ++i) {
                        if (players[i].clientId == event.clientId) {
                            playerId = i;
                            break;
                        }
                    }

                    if (playerId == -1) return;

                    printf("Room %d: client %d submitted '%s'\n", id, event.clientId, event.inputStr.data());

                    auto & player = players[playerId];

                    bool correct = false;
                    for (auto & query : pool) {
                        if (event.inputStr == query.answer) {
                            player.score += 3.0f + 0.2f*query.answer.size();
                            correct = true;
                            query.clientId = event.clientId;
                            query.tGuessed_s = ::timestamp_s();
                            poolOld.emplace_back(std::move(query));
                            query = generate();
                            printf("correct!\n");

                            break;
                        }
                    }

                    if (correct == false) {
                        printf("wrong!\n");
                        player.score -= 1.0f;
                        if (player.score < 0.0) player.score = 0.0f;
                    }
                }
                break;
            default:
                {
                    printf("Unhandled event, type = %d\n", event.type);
                }
                break;
        };
    }
};
