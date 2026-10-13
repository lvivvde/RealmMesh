#include "realmmesh/test_support/mongod_process.hpp"

#include <iostream>

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string_view{argv[1]} == "--shared") {
            std::cout << realm::test_support::MongodProcess::shared().eval(argv[2], argv[3]) << '\n';
            return 0;
        }
        realm::test_support::MongodProcess process;
        std::cout << process.eval(
            realm::test_support::MongodProcess::fresh_database(),
            "const hello = db.hello(); print(hello.setName + ':' + hello.isWritablePrimary)")
                  << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
