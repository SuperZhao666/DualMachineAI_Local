#include "vfdual/bgra_nv12.hpp"
#include "vfdual/bgra_resize.hpp"
#include <array>
#include <chrono>
#include <iostream>
#include <random>
#include <vector>

int main(){
    std::mt19937 random(6401024);std::size_t cases=0;
    for(unsigned width=2;width<=70;width+=2)for(unsigned height: {2U,6U,18U})
        for(unsigned padding: {0U,1U,17U,64U}){
            const auto pitch=width*4+padding,size=width*height*3/2;
            std::vector<std::uint8_t> source(1+pitch*height);
            for(auto& x:source)x=static_cast<std::uint8_t>(random());
            const auto original=source;
            std::vector<std::uint8_t> a(size+47,0xa5),b=a;
            vfdual::bgra_to_nv12_scalar(source.data()+1,pitch,width,height,a.data()+19);
            vfdual::bgra_to_nv12_sse2(source.data()+1,pitch,width,height,b.data()+19);
            if(a!=b || source!=original){std::cerr<<"mismatch "<<width<<'x'<<height<<" pitch="<<pitch;return 1;}
            ++cases;
        }
    std::array<std::uint8_t,16> black{};std::array<std::uint8_t,6> output{};
    vfdual::bgra_to_nv12_sse2(black.data(),8,2,2,output.data());
    if(output!=std::array<std::uint8_t,6>{16,16,16,16,128,128})return 2;
    constexpr unsigned width=640,height=640,pitch=width*4;
    std::vector<std::uint8_t> source(pitch*height),a(width*height*3/2),b(a.size());
    for(auto& x:source)x=static_cast<std::uint8_t>(random());
    std::vector<long long> scalar,simd;
    for(unsigned i=0;i<220;++i){
        auto t=std::chrono::steady_clock::now();
        vfdual::bgra_to_nv12_scalar(source.data(),pitch,width,height,a.data());
        auto t1=std::chrono::steady_clock::now();
        vfdual::bgra_to_nv12_sse2(source.data(),pitch,width,height,b.data());
        auto t2=std::chrono::steady_clock::now();
        if(a!=b)return 3;
        if(i>=20){scalar.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1-t).count());
            simd.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t2-t1).count());}
    }
    std::sort(scalar.begin(),scalar.end());std::sort(simd.begin(),simd.end());
    std::cout<<"PASS cases="<<cases+221<<" exact_pixels=true padding_and_bounds=true"
        <<" scalar_p50_us="<<scalar[100]/1000.<<" scalar_p95_us="<<scalar[190]/1000.
        <<" sse2_p50_us="<<simd[100]/1000.<<" sse2_p95_us="<<simd[190]/1000.<<'\n';
    vfdual::BgraResizePlan plan;
    if(plan.configure(1,1,0,0) || plan.configure(2,2,4,4))return 4;
    constexpr unsigned sw=1024,sh=1024,sp=sw*4+13;
    std::vector<std::uint8_t> large(sp*sh),small(width*height*4+32,0xcd);
    for(auto& x:large)x=static_cast<std::uint8_t>(random());
    if(!plan.configure(sw,sh,width,height))return 5;
    plan.run(large.data(),sp,small.data()+16);
    double absolute=0;int maximum=0;
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
        const double xx=(x+.5)*sw/width-.5,yy=(y+.5)*sh/height-.5;
        const auto x0=static_cast<unsigned>(xx),y0=static_cast<unsigned>(yy);const auto wx=xx-x0,wy=yy-y0;
        for(unsigned c=0;c<4;++c){
            const auto at=[&](unsigned dx,unsigned dy){return large[(y0+dy)*sp+(x0+dx)*4+c];};
            const int reference=static_cast<int>(std::round((at(0,0)*(1-wx)+at(1,0)*wx)*(1-wy)+(at(0,1)*(1-wx)+at(1,1)*wx)*wy));
            const auto error=std::abs(reference-int(small[16+(y*width+x)*4+c]));
            maximum=std::max(maximum,error);absolute+=error;
        }
    }
    if(maximum>1 || !std::all_of(small.begin(),small.begin()+16,[](auto x){return x==0xcd;}) ||
       !std::all_of(small.end()-16,small.end(),[](auto x){return x==0xcd;}))return 6;
    std::vector<long long> resize_times;
    for(unsigned i=0;i<120;++i){const auto t=std::chrono::steady_clock::now();plan.run(large.data(),sp,small.data()+16);
        const auto done=std::chrono::steady_clock::now();
        if(i>=20)resize_times.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(done-t).count());}
    std::sort(resize_times.begin(),resize_times.end());
    std::cout<<"PASS resize_float_reference_max_lsb="<<maximum<<" mean_lsb="<<absolute/(width*height*4)
        <<" resize_p50_us="<<resize_times[50]/1000.<<" resize_p95_us="<<resize_times[95]/1000.<<'\n';
}
