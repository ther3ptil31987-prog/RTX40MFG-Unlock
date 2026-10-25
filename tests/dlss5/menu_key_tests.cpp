#include "optiscaler_menu_key.h"
#include <cstdio>
#include <cstdlib>
unsigned checks=0;
#define CHECK(c) do{++checks;if(!(c)){std::fprintf(stderr,"FAIL line %u: %s\n",__LINE__,#c);std::exit(1);}}while(0)
using namespace optiscaler_menu_key;
int main(){
 // OptiScaler's Config::readInt: hex with 0x, decimal, the whole value.
 CHECK(ParseShortcutKey(L"0x2e")==VK_DELETE);CHECK(ParseShortcutKey(L"0X2E")==VK_DELETE);
 CHECK(ParseShortcutKey(L"46")==VK_DELETE);CHECK(ParseShortcutKey(L" 0x2d\t")==VK_INSERT);
 // Absent, auto or unreadable: OptiScaler's default, Insert.
 CHECK(ParseShortcutKey(L"")==kDefaultKey);CHECK(ParseShortcutKey(L"auto")==kDefaultKey);CHECK(ParseShortcutKey(L"AUTO")==kDefaultKey);
 CHECK(ParseShortcutKey(L"0x2e ; delete")==kDefaultKey);CHECK(ParseShortcutKey(L"2e")==kDefaultKey);
 CHECK(ParseShortcutKey(L"0x")==kDefaultKey);CHECK(ParseShortcutKey(L"99999999999")==kDefaultKey);
 // Read but outside 1-255: OptiScaler has no menu key.
 CHECK(ParseShortcutKey(L"0")==0);CHECK(ParseShortcutKey(L"-1")==0);CHECK(ParseShortcutKey(L"256")==0);CHECK(ParseShortcutKey(L"0x100")==0);
 CHECK(ParseShortcutKey(L"0xff")==255);CHECK(ParseShortcutKey(L"1")==1);

 // An OptiScaler that hides the held key while its menu is open: this menu
 // follows OptiScaler's menu on every press, whatever state it was left in.
 Follower f;
 CHECK(f.AfterPress(true,false)==true&&!f.BlockingSeen());   // both closed: both open
 CHECK(f.AfterPress(false,true)==false&&f.BlockingSeen());   // OptiScaler's was open: both close
 CHECK(f.AfterPress(true,false)==true);                      // both open
 CHECK(f.AfterPress(true,true)==true);                       // OptiScaler's menu closed by its Close button: both open
 CHECK(f.AfterPress(false,false)==false);                    // this menu closed by its X: both stay closed
 CHECK(f.AfterPress(false,true)==false);
 // A build that never hides the key: plain toggle with this menu.
 Follower plain;
 CHECK(plain.AfterPress(true,false)==true);CHECK(plain.AfterPress(true,true)==false);CHECK(plain.AfterPress(true,false)==true);
 CHECK(!plain.BlockingSeen());
 std::printf("menu key: %u checks\n",checks);
 return 0;
}
