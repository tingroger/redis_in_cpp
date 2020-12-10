#include "../src/sds.h"
#include <iostream>
using namespace std;

int main()
{
    sdshdr<int> a;
    cout << sizeof(a) << endl;
    return 0;
}