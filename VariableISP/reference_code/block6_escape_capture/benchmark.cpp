#include "escape.hpp"
#include <chrono>
#include <iostream>
#include <vector>
#include <algorithm>
int main(){
 std::vector<double> ms;double sum=0;
 for(int i=0;i<300;++i){
  double h=490+(2480.-490)*(i%100)/99;
  auto t=std::chrono::steady_clock::now();
  auto s=escape_capture::minimum_effort(.5,h,.01,10,50);
  auto f=std::chrono::steady_clock::now();sum+=s.effort;
  ms.push_back(std::chrono::duration<double,std::milli>(f-t).count());
 }
 std::sort(ms.begin(),ms.end());double avg=0;for(double v:ms)avg+=v;
 std::cout<<"{\"queries\":300,\"mean_ms\":"<<avg/ms.size()<<",\"median_ms\":"<<ms[150]
          <<",\"p95_ms\":"<<ms[285]<<",\"max_ms\":"<<ms.back()<<",\"checksum\":"<<sum<<"}\n";
}
