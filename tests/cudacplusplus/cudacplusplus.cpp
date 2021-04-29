#include <iostream>
#include <vector>

#include "luxrays/utils/cudacpp.h"

using namespace std;
using namespace luxrays;

//------------------------------------------------------------------------------
// DevicesInfo()
//------------------------------------------------------------------------------

void DevicesInfo() {
	int count;

	cudaGetDeviceCount(&count);
	CUDACPP_CHECKERROR();

	for (int i = 0; i < count; i++) {
		cudaDeviceProp prop;
		cudaGetDeviceProperties(&prop, i);
		CUDACPP_CHECKERROR();

		cout << "Device Number: " << i << endl;
		cout << "  Device name: " << prop.name << endl;
		cout << "  Memory Clock Rate (KHz): " << prop.memoryClockRate << endl;
		cout << "  Memory Bus Width (bits): " << prop.memoryBusWidth << endl;
		cout << "  Compute Capability: " << prop.major << "." << prop.minor <<endl;
	}
}
  
//------------------------------------------------------------------------------
// ClassTest()
//------------------------------------------------------------------------------

class VectorTest {
public:
	__host__ __device__ VectorTest(float _x = 0.f, float _y = 0.f, float _z = 0.f) :
		x(_x), y(_y), z(_z) {
	}
		
	__host__ __device__ VectorTest operator+(const VectorTest &v) const {
		return VectorTest(x + v.x, y + v.y, z + v.z);
	}
	
	__host__ __device__ bool operator!=(const VectorTest &v) const {
		return (x != v.x) || (y != v.y) || (z != v.z);
	}

	float x, y, z;
};

__global__ void VectorTestKernel(VectorTest *va, VectorTest *vb, VectorTest *vc) {
	const u_int index = blockIdx.x * blockDim.x + threadIdx.x;

	vc[index] = va[index] + vb[index];
}

inline ostream &operator<<(std::ostream &os, const VectorTest &v) {
	os << "VectorTest[" << v.x << ", " << v.y << ", " << v.z << "]";
	return os;
}

void ClassTest() {
	cout << "Class test..." << endl;

	const size_t size = 1024;

	VectorTest *va = CudaCPPHostNewArray<VectorTest>(size);
	VectorTest *vb = CudaCPPHostNewArray<VectorTest>(size);
	VectorTest *vc = CudaCPPHostNewArray<VectorTest>(size);

	for (u_int i = 0; i < size; ++i) {
		va[i] = VectorTest(i, 0.f, 0.f);
		vb[i] = VectorTest(0.f, i, 0.f);
		vc[i] = VectorTest(0.f, 0.f, 0.f);
	}

	VectorTestKernel<<<size / 32, 32>>>(&va[0], &vb[0], &vc[0]);
	CUDACPP_CHECKERROR();

	cudaDeviceSynchronize();
	CUDACPP_CHECKERROR();

	for (u_int i = 0; i < size; ++i) {
		if (vc[i] != va[i] + vb[i])
			cout << "Failed index: " << i << endl;
		
		//cout << vc[i] << " = " << va[i] << " + " << vb[i] << endl;
	}

	CudaCPPHostDeleteArray<VectorTest>(va, size);
	CudaCPPHostDeleteArray<VectorTest>(vb, size);
	CudaCPPHostDeleteArray<VectorTest>(vc, size);

	cout << "Done" << endl;
}

//------------------------------------------------------------------------------
// VirtualMethodTest()
//------------------------------------------------------------------------------

typedef enum {
	MATERIAL_TEST_MATTE, MATERIAL_TEST_MIRROR
} MaterialType;

class MaterialTest {
public:
	__host__ __device__ MaterialTest() {
	}

	__host__ __device__ MaterialType GetType() { return type; }

	__device__ float Evaluate(const float v) const;
	
protected:
	MaterialType type;
};

class MatteMaterialTest : public MaterialTest {
public:
	__host__ MatteMaterialTest(float t1, int t2) {
		type = MATERIAL_TEST_MATTE;
	}
	
	friend class MaterialTest;

protected:
	__host__ __device__ float EvaluateImpl(const float v) const { return v; }
};

class MirrorMaterialTest : public MaterialTest {
public:
	__host__ MirrorMaterialTest() {
		type = MATERIAL_TEST_MIRROR;
	}
	
	friend class MaterialTest;

protected:
	__host__ __device__ float EvaluateImpl(const float v) const { return v + 1.f; }
};

__device__ float MaterialTest::Evaluate(const float v)  const {
	switch (type) {
		case MATERIAL_TEST_MATTE:
			return ((const MatteMaterialTest *)this)->EvaluateImpl(v);
		case MATERIAL_TEST_MIRROR:
			return ((const MirrorMaterialTest *)this)->EvaluateImpl(v);
		default:
#if defined(__CUDA_ARCH__)
			return 123.f;
#else
			throw std::runtime_error("Unknown material type in MaterialTest::Evaluate()");
#endif
	}
}

__global__ void VirtualMethodTestKernel(MaterialTest **vm, MaterialType *vt, float *vv) {
	const u_int index = blockIdx.x * blockDim.x + threadIdx.x;
	
	vt[index] = vm[index]->GetType();
	vv[index] = vm[index]->Evaluate(0.f);
}

void VirtualMethodTest() {
	cout << "Virtual method test..." << endl;

	const size_t size = 32;

	MaterialTest **vm = CudaCPPHostNewArray<MaterialTest *>(size);
	for (u_int i = 0; i < size; ++i) {
		if (i % 2 == 0)
			vm[i] = CudaCPPHostNew<MatteMaterialTest>(1.f, 2.f);
		else
			vm[i] = CudaCPPHostNew<MirrorMaterialTest>();
	}

	MaterialType *vt = CudaCPPHostNewArray<MaterialType>(size);
	float *vv = CudaCPPHostNewArray<float>(size);
	
	VirtualMethodTestKernel<<<size / 32, 32>>>(vm, vt, vv);
	CUDACPP_CHECKERROR();

	cudaDeviceSynchronize();
	CUDACPP_CHECKERROR();

	for (u_int i = 0; i < size; ++i) {
		if (((i % 2 == 0) && (vt[i] != MATERIAL_TEST_MATTE)) ||
				((i % 2 == 1) && (vt[i] != MATERIAL_TEST_MIRROR)))
			cout << "Failed GetType() index: " << i << " (value = " << ToString(vt[i]) << ")" << endl;
	}

	for (u_int i = 0; i < size; ++i) {
		if (((i % 2 == 0) && (vv[i] != 0.f)) ||
				((i % 2 == 1) && (vv[i] != 1.f)))
			cout << "Failed Evaluate() index: " << i << " (value = " << ToString(vv[i]) << ")" << endl;
	}

	for (u_int i = 0; i < size; ++i) {
		if (vm[i]->GetType() == MATERIAL_TEST_MATTE)
			CudaCPPHostDelete<MatteMaterialTest>(vm[i]);
		else
			CudaCPPHostDelete<MirrorMaterialTest>(vm[i]);
	}
	
	CudaCPPHostDeleteArray<MaterialTest *>(vm, size);
	CudaCPPHostDeleteArray<MaterialType>(vt, size);
	CudaCPPHostDeleteArray<float>(vv, size);

	cout << "Done" << endl;
}

//------------------------------------------------------------------------------

// This is required ot be able to use ToString()
namespace luxrays {
std::locale cLocale("C");
}

int main(int argc, char ** argv) {
	cout << "CUDA C++ Tests" << endl;

	DevicesInfo();
	//cudaSetDevice(0);
	//CUDACPP_CHECKERROR();
	
	ClassTest();
	VirtualMethodTest();

	return 0;
}
