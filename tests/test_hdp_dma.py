"""Lifecycle and ordering against the actual linked C++ implementation, CPU recording endpoints."""
import unittest
import test_pre_kc as fixture
FIXTURE=r'''
static std::array<uint8_t,0xec0> testGvm;
static std::array<uint8_t,0x68> testHdp;
static unsigned nativeInitCalls,nativeHwExitCalls,nativeSwExitCalls;
static uint32_t nativeInitStatus,nativeExitStatus,hdpReadback;
static std::vector<unsigned> hdpEvents;
template<class T> static void set(void *p,size_t at,T v){memcpy(static_cast<uint8_t *>(p)+at,&v,sizeof(v));}
static void *discovery(void *gvm,uint32_t ip){assert(gvm==testGvm.data()&&ip==34);return testGvm.data()+8;}
static uint32_t offset(void *record,uint32_t instance,uint32_t segment,uint32_t reg){assert(record==testGvm.data()+8&&!instance&&!segment&&reg==0xd1);return 0x40d1;}
static void hdpWrite(void *context,uint32_t reg,uint32_t value,uint32_t type){assert(context==&dmaContext&&reg==0x40d1&&value==1&&type==34);hdpEvents.push_back(1);}
static uint32_t hdpRead(void *context,uint32_t reg,uint32_t type){assert(context==&dmaContext&&reg==0x40d1&&type==34);hdpEvents.push_back(2);memset(dmaMemory,0x6b,sizeof(dmaMemory));return hdpReadback;}
static uint32_t nativeInit(void *gvm,void *,void *){assert(gvm==testGvm.data());++nativeInitCalls;return nativeInitStatus;}
static uint32_t nativeHwExit(void *gvm){assert(gvm==testGvm.data()&&!gContext.hdp.live);++nativeHwExitCalls;hdpEvents.push_back(3);return nativeExitStatus;}
static uint32_t nativeSwExit(void *gvm){assert(gvm==testGvm.data()&&!gContext.hdp.live);++nativeSwExitCalls;return nativeExitStatus;}
static void setup(){
 testGvm={};testHdp={};dmaContext[1]=&dmaToken;dmaProvider.writeRegister=hdpWrite;dmaProvider.readRegister=hdpRead;
 set(testGvm.data(),0xe48,&dmaProvider);set(testGvm.data(),0xe90,testHdp.data());
 set(testHdp.data(),0,testGvm.data());set<uint32_t>(testHdp.data(),8,0x00020005);
 auto r=testGvm.data()+8;set<uint32_t>(r,0,34);set<uint32_t>(r,4,1);set<uint32_t>(r,8,5);set<uint32_t>(r,12,2);set<uint32_t>(r,0x2c,0x4000);
 gGvmServices={discovery,offset};orgGvmHwInit=reinterpret_cast<uintptr_t>(nativeInit);
 orgGvmHwExit=reinterpret_cast<uintptr_t>(nativeHwExit);orgGvmSwExit=reinterpret_cast<uintptr_t>(nativeSwExit);
 gExitHooksReady=true;rem660::smu::SwInput input{};input.size=sizeof(input);input.required8=1;input.major=13;input.revision=3;
 bindInput(input);rem660::smu::SwOutput output{};output.size=sizeof(output);assert(!smuSwInit(&input,&output));
}
'''
class HdpDma(unittest.TestCase):
 host=fixture.PreKc.host
 smu=fixture.PreKc.smu
 def test_powerup_reuses_owned_hdp_dma_without_claiming_completion(self):
  # This replacement supplies CPU fixture bytes at the existing HDP recorder;
  # no driver code or PMFW result is replaced by a success implementation.
  source=(self.smu()+FIXTURE).replace('memset(dmaMemory,0x6b,sizeof(dmaMemory));',
   'rem660::smu::DpmClocks clocks{};clocks.minGfx=300;clocks.maxGfx=1500;clocks.dfCount=2;clocks.df[0].memory=1600;clocks.df[1].memory=800;clocks.socCount=2;clocks.soc[0]=400;clocks.soc[1]=800;memcpy(dmaMemory,&clocks,sizeof(clocks));')
  self.host(source+r'''
int main(){using namespace rem660::smu;setup();featureLow=YellowCarpGfxDpm;
 assert(!wrapGvmHwInit(testGvm.data(),nullptr,nullptr));
 for(uint32_t id:{0u,4u,6u,10u}){EventInput event{16,id,nullptr};uint64_t output=0x1234;
  assert(!dispatchEvent(&gContext,&event,&output));
  assert(gContext.powerUp.prepared&&gContext.powerUp.clocks.minGfx==300&&allocationCalls==1&&!gContext.table.lease.inFlight);
 }
 auto commands=writes.size();EventInput event{16,0,nullptr};gContext.operationLock=1;
 assert(dispatchEvent(&gContext,&event,nullptr)==NotReady&&writes.size()==commands);gContext.operationLock=0;
 assert(!wrapGvmHwExit(testGvm.data()));
 assert(!smuSwExit(&gContext)&&!gContext.powerUp.prepared);
 assert(dispatchEvent(&gContext,&event,nullptr)==NotReady);
}
''')
 def test_power_budget_uses_existing_dma_and_never_publishes_stapm_as_ppt(self):
  source=(self.smu()+FIXTURE).replace('memset(dmaMemory,0x6b,sizeof(dmaMemory));',
   'rem660::smu::Metrics metrics{};metrics.stapmOperatingWatts=28;metrics.stapmCurrentWatts=25;memcpy(dmaMemory,&metrics,sizeof(metrics));')
  self.host(source+r'''
int main(){using namespace rem660::smu;setup();assert(!wrapGvmHwInit(testGvm.data(),nullptr,nullptr));
 PowerLimitInput in{};uint32_t output=0x12345678;
 assert(!dispatchPower(&gContext,2,&in,&output)&&output==35000);
 assert(gContext.powerBudget.valid&&gContext.powerBudget.operatingWatts==28&&gContext.powerBudget.currentWatts==25&&allocationCalls==1);
 auto commands=writes.size();gContext.operationLock=1;
 assert(dispatchPower(&gContext,2,&in,&output)==NotReady&&writes.size()==commands);gContext.operationLock=0;
 int foreign;assert(dispatchPower(&foreign,2,&in,&output)==NotReady&&writes.size()==commands);
 assert(!wrapGvmHwExit(testGvm.data()));
 assert(!smuSwExit(&gContext));assert(dispatchPower(&gContext,2,&in,&output)==NotReady);
}
''')
 def test_postinit_cache_contract_rejected_before_dma(self):
  self.host(self.smu()+FIXTURE+r'''
int main(){using namespace rem660::smu;setup();assert(!wrapGvmHwInit(testGvm.data(),nullptr,nullptr));
 dmaToken=1;std::array<uint8_t,304> output;output.fill(0xa5);auto before=output;auto commands=writes.size();
 assert(readOwnedSmuTable(&gContext.table,4,output.data(),output.size())==Unsupported&&output==before&&writes.size()==commands&&hdpEvents.empty());
 assert(gContext.table.owned&&!gContext.table.usable&&allocationCalls==1);
 assert(readOwnedSmuTable(&gContext.table,4,output.data(),output.size())==NotReady&&allocationCalls==1);
 assert(!wrapGvmHwExit(testGvm.data())&&releaseCalls==1&&nativeHwExitCalls==1);
}
''')
 def test_hdp_real_binding_order_native_failure_and_teardown(self):
  self.host(self.smu()+FIXTURE+r'''
int main(){using namespace rem660::smu;setup();nativeInitStatus=4;
 assert(wrapGvmHwInit(testGvm.data(),nullptr,nullptr)==4&&!gContext.hdp.gvm);
 nativeInitStatus=0;assert(!wrapGvmHwInit(testGvm.data(),nullptr,nullptr)&&gContext.hdp.live);
 std::array<uint8_t,304> output;output.fill(0xa5);auto before=output;
 assert(!readOwnedSmuTable(&gContext.table,4,output.data(),output.size()));
 assert(output[0]==0x6b&&hdpEvents==std::vector<unsigned>({1,2})&&allocationCalls==1);
 assert(!readOwnedSmuTable(&gContext.table,4,output.data(),output.size())&&allocationCalls==1);
 auto commands=writes.size();gContext.operationLock=1;
 assert(wrapGvmHwExit(testGvm.data())==NotReady&&!nativeHwExitCalls&&!releaseCalls);gContext.operationLock=0;
 nativeExitStatus=3;assert(wrapGvmHwExit(testGvm.data())==3&&nativeHwExitCalls==1&&releaseCalls==1&&!gContext.hdp.live&&!gContext.table.owned);
 assert(hdpEvents.back()==3&&!gContext.table.lease.owner);
 assert(readOwnedSmuTable(&gContext.table,4,output.data(),output.size())==NotReady&&writes.size()==commands);
 nativeExitStatus=0;assert(!wrapGvmSwExit(testGvm.data())&&nativeSwExitCalls==1);
 assert(prepareGvmHdp(testGvm.data(),gGvmServices,gContext.hdp)==NotReady); // no stale-pointer rebind
 assert(before[0]==0xa5); // CPU inputs never mistaken for hardware evidence
}
''')
 def test_hdp_failed_readback_and_cleanup_quarantine(self):
  self.host(self.smu()+FIXTURE+r'''
int main(){using namespace rem660::smu;setup();assert(!wrapGvmHwInit(testGvm.data(),nullptr,nullptr));
 std::array<uint8_t,304> output;output.fill(0xa5);auto before=output;hdpReadback=UINT32_MAX;
 assert(readOwnedSmuTable(&gContext.table,4,output.data(),output.size())==NotReady&&output==before&&gContext.table.lease.inFlight);
 auto commands=writes.size();assert(wrapGvmSwExit(testGvm.data())==NotReady&&!nativeSwExitCalls&&!releaseCalls);
 assert(gContext.phase==5&&!gContext.hdp.live&&gContext.table.owned);
 assert(smuSwExit(&gContext)==Invalid&&!releaseCalls); // no later dereference of retired borrowed CGS
 assert(readOwnedSmuTable(&gContext.table,4,output.data(),output.size())==NotReady&&writes.size()==commands);
}
''')
 def test_hdp_bad_discovery_cgs_and_release_error(self):
  self.host(self.smu()+FIXTURE+r'''
int main(){using namespace rem660::smu;setup();HdpBinding b{};
 set<uint32_t>(testGvm.data()+8,12,0);assert(prepareGvmHdp(testGvm.data(),gGvmServices,b)==Unsupported&&!b.gvm);
 set<uint32_t>(testGvm.data()+8,12,2);set<uint32_t>(testGvm.data()+8,0x2c,UINT32_MAX);
 assert(prepareGvmHdp(testGvm.data(),gGvmServices,b)==Invalid&&!b.gvm);set<uint32_t>(testGvm.data()+8,0x2c,0x4000);
 dmaProvider.readRegister=nullptr;assert(prepareGvmHdp(testGvm.data(),gGvmServices,b)==NotReady);dmaProvider.readRegister=hdpRead;
 assert(!wrapGvmHwInit(testGvm.data(),nullptr,nullptr));
 OwnedDmaTable other{};CgsPrefix foreign=dmaProvider;other.cgs=&foreign;
 assert(bindOwnedHdp(other,gContext.hdp)==NotReady&&!other.lease.owner);
 std::array<uint8_t,304> output{};assert(!readOwnedSmuTable(&gContext.table,4,output.data(),output.size()));
 releaseError=0xe00002c7;assert(wrapGvmHwExit(testGvm.data())==releaseError&&!nativeHwExitCalls&&releaseCalls==1&&gContext.phase==5&&gContext.table.owned);
 assert(smuSwExit(&gContext)==Invalid&&releaseCalls==1);
}
''')
