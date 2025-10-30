#include "views/viewer.hpp"

#ifdef _DEBUG
int main(int, char *)
#else
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
#endif
{
    const auto win = std::make_unique<Viewer>();
    win->Show();
    return sw::App::MsgLoop();
}