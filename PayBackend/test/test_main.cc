#define DROGON_TEST_MAIN
#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include "../utils/ConfigLoader.h"
#include <fstream>
#include <json/json.h>

int main(int argc, char **argv)
{
    using namespace drogon;

    // Load .env and process config.json placeholders (same as main.cc)
    ConfigLoader::loadEnvFile(".env");

    std::ifstream configFile("./config.json");
    if (configFile.is_open())
    {
        Json::Value config;
        Json::CharReaderBuilder builder;
        std::string errors;
        if (Json::parseFromStream(builder, configFile, &config, &errors))
        {
            Json::Value processedConfig = ConfigLoader::loadConfig(config);
            app().loadConfigJson(std::move(processedConfig));
        }
    }

    std::promise<void> p1;
    std::future<void> f1 = p1.get_future();

    // Start the main loop on another thread
    std::thread thr([&]() {
        // Queues the promise to be fulfilled after starting the loop
        app().getLoop()->queueInLoop([&p1]() { p1.set_value(); });
        app().run();
    });

    // The future is only satisfied after the event loop started
    f1.get();
    int status = test::run(argc, argv);

    // Ask the event loop to shutdown and wait
    app().getLoop()->queueInLoop([]() { app().quit(); });
    thr.join();
    return status;
}
