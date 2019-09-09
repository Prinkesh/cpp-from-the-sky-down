#include <memory>
#include <functional>
struct Dummy {};

class draw;
int poly_extend(draw*, Dummy&);

struct Base {
	virtual int draw() = 0;
	virtual ~Base() {}
};


std::function<int(Dummy&)> GetFunction();
std::unique_ptr<Base> MakeBase();