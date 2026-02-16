#include <voie/voie.h>

int main() {
    voie::app app;

    app.get("/", voie::prebuilt("hello, world", "text/plain"));

    app.listen(8080);
}
