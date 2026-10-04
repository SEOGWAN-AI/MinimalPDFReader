#include "../source/layout.hpp"
#include <cassert>
#include <iostream>
#include <random>

static void near(double a,double b,double error=1e-7) { assert(std::abs(a-b)<error); }
int main() {
    reader::Layout v;
    v.pages={{595,842},{842,595},{200,200},{300,1500}};
    v.width=1440; v.height=900; v.reflow();
    for(size_t i=0;i<v.pages.size();++i) {
        const auto r=v.rect(i);
        near(r.width,1440); near(r.x,0);
        near(r.height/r.width,v.pages[i].height/v.pages[i].width);
        if(i) assert(v.tops[i]>v.tops[i-1]);
    }
    v.scrollY=v.tops[1]+200;
    const reader::Point focal{700,450};
    auto before=v.capture(focal);
    v.zoomAt(2.25,focal);
    auto after=v.capture(focal);
    assert(before.page==after.page); near(before.x,after.x); near(before.y,after.y);
    v.zoomAt(1,focal);
    near(v.scrollX,0); near(v.rect(0).width,1440);
    v.scrollY=v.tops[2]+100;
    before=v.capture({v.width/2,v.height/2});
    v.resize(1920,1080);
    after=v.capture({v.width/2,v.height/2});
    assert(before.page==after.page); near(before.y,after.y);
    near(v.rect(0).width,1920);
    v.pan(1e9,1e9); near(v.scrollY,v.maxY()); near(v.scrollX,v.maxX());
    v.pan(-1e9,-1e9); near(v.scrollX,0); near(v.scrollY,0);
    v.zoomAt(1e9,focal); near(v.zoom,8);
    v.zoomAt(-1,focal); near(v.zoom,.25);
    v.zoomAt(std::nan(""),focal); near(v.zoom,.25);
    reader::Layout empty; empty.resize(1,1); empty.zoomAt(2,{0,0});
    assert(empty.visible().empty());
    std::mt19937 random(12345);
    for(int trial=0;trial<10000;++trial) {
        const reader::Size page{double(100+random()%1600),double(100+random()%2400)};
        const double target=double(50+random()%50000);
        const auto raster=reader::rasterSize(page,target);
        assert(raster.width>=1 && raster.height>=1);
        assert(raster.width<=8192 && raster.height<=8192);
        assert(raster.width*raster.height<=16000000);
        reader::Layout x;
        x.width=400+random()%3400; x.height=300+random()%1500;
        x.pages={page,{600,900},{900,600}}; x.reflow();
        x.zoomAt(1+(random()%500)/100.0,{x.width/2,x.height/2});
        x.pan(double(random()%100000),double(random()%100000));
        assert(x.scrollX>=0 && x.scrollX<=x.maxX());
        assert(x.scrollY>=0 && x.scrollY<=x.maxY());
        for(auto i:x.visible()) assert(i<x.pages.size());
    }
    std::cout << "PASS: width fit, mixed pages, focal zoom, resize anchor, scroll bounds, "
                 "zoom bounds, 10000 randomized raster/layout cases\n";
}
