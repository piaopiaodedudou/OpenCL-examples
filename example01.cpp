#include <iostream>
#include "CL/cl.hpp"
#include <ctime>


double timeAddVectorsCPU(int n, int k) {
    // adds two vectors of size n, k times, returns total duration
    std::clock_t start;
    double duration;

    int A[n], B[n], C[n];
    for (int i=0; i<n; i++) {
        A[i] = i;
        B[i] = n-i;
        C[i] = 0;
    }

    start = std::clock();
    for (int i=0; i<k; i++) {
        for (int j=0; j<n; j++) {
            C[j] = A[j] + B[j];
        }
    }

    duration = (std::clock() - start) / (double) CLOCKS_PER_SEC;
    return duration;
}

int main() {
    // get all platforms (drivers), e.g. NVIDIA
    std::vector<cl::Platform> all_platforms;
    cl::Platform::get(&all_platforms);

    if (all_platforms.size()==0) {
        std::cout<<" No platforms found. Check OpenCL installation!\n";
        exit(1);
    }
    cl::Platform default_platform=all_platforms[0];
    // std::cout << "Using platform: "<<default_platform.getInfo<CL_PLATFORM_NAME>()<<"\n";

    // get default device (CPUs, GPUs) of the default platform
    std::vector<cl::Device> all_devices;
    default_platform.getDevices(CL_DEVICE_TYPE_ALL, &all_devices);
    if(all_devices.size()==0){
        std::cout<<" No devices found. Check OpenCL installation!\n";
        exit(1);
    }

    // use device[1] because that's a GPU; device[0] is the CPU
    cl::Device default_device=all_devices[1];
    // std::cout<< "Using device: "<<default_device.getInfo<CL_DEVICE_NAME>()<<"\n";

    cl::Context context({default_device});
    cl::Program::Sources sources;

    // calculates for each element; C = A + B
    std::string kernel_code=
        //  is equivalent to the host's "time_add_vectors" function, except the
        //  timing will be done on the host.
        "   void kernel looped_add(global const int* v1, global const int* v2, global int* v3, "
        "                          global const int* constants) {"
        "       int ID, Nthreads, n, k, ratio, start, stop;"
        "       ID = get_global_id(0);"
        "       Nthreads = get_global_size(0);"
        "       n = constants[0];"  // size of vectors
        "       k = constants[1];"  // number of loop iterations
        ""
        "       ratio = (n / Nthreads);" // elements per thread
        "       start = ratio * ID;"
        "       stop  = ratio * (ID+1);"
        ""
        "       int i, j;" // will the compiler optimize this anyway? probably.
        "       for (i=0; i<k; i++) {"
        "           for (j=start; j<stop; j++)"
        "               v3[j] = v1[j] + v2[j];"
        "       }"
        "   }"
        ""
        "   void kernel add(global const int* v1, global const int* v2, global int* v3, "
        "                   global const int* constants) {"
        "       int ID, Nthreads, n, ratio, start, stop;"
        "       ID = get_global_id(0);"
        "       Nthreads = get_global_size(0);"
        "       n = constants[0];"
        ""
        "       ratio = (n / Nthreads);"
        "       start = ratio * ID;"
        "       stop  = ratio * (ID+1);"
        ""
        "       for (int i=start; i<stop; i++)"
        "           v3[i] = v1[i] + v2[i];"
        "   }";
    sources.push_back({kernel_code.c_str(), kernel_code.length()});

    cl::Program program(context, sources);
    if (program.build({default_device}) != CL_SUCCESS) {
        std::cout << "Error building: " << program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(default_device) << std::endl;
        exit(1);
    }

    int n, k, Nthreads; 
    n = 100000;  // size of vectors
    k = 1000;   // number of loop iterations
    Nthreads = 10;
    int constants[2] = {n, k};

    // run the CPU code
    float CPUtime = timeAddVectorsCPU(n, k);

    // run some GPU code; this block allocates space, writes buffers, and then
    // adds the same two vectors multiple times -- i.e. it's equivalent to the
    // host (CPU) code above, but with some necessary overhead.
    cl::CommandQueue queue(context, default_device);
    cl::KernelFunctor looped_add(cl::Kernel(program, "looped_add"), queue, cl::NullRange, cl::NDRange(Nthreads), cl::NullRange);
    cl::KernelFunctor add(cl::Kernel(program, "add"), queue, cl::NullRange, cl::NDRange(Nthreads), cl::NullRange);

    // construct vectors
    int A[n], B[n], C[n];
    for (int i=0; i<n; i++) {
        A[i] = i;
        B[i] = n - i - 1;
        C[i] = 0;
    }

    // start timer
    double GPUtime1;
    std::clock_t start_time;
    start_time = std::clock();

    // allocate space
    cl::Buffer buffer_A(context, CL_MEM_READ_WRITE, sizeof(int) * n);
    cl::Buffer buffer_B(context, CL_MEM_READ_WRITE, sizeof(int) * n);
    cl::Buffer buffer_C(context, CL_MEM_READ_WRITE, sizeof(int) * n);
    cl::Buffer buffer_constants(context, CL_MEM_READ_ONLY, sizeof(int) * 2);

    // push write commands to queue
    queue.enqueueWriteBuffer(buffer_A, CL_TRUE, 0, sizeof(int)*n, A);
    queue.enqueueWriteBuffer(buffer_B, CL_TRUE, 0, sizeof(int)*n, B);
    queue.enqueueWriteBuffer(buffer_constants, CL_TRUE, 0, sizeof(int)*2, constants);

    // RUN ZE KERNEL
    looped_add(buffer_A, buffer_B, buffer_C, buffer_constants);

    // read result from GPU to here; including for the sake of timing
    queue.enqueueReadBuffer(buffer_C, CL_TRUE, 0, sizeof(int)*n, C);
    GPUtime1 = (std::clock() - start_time) / (double) CLOCKS_PER_SEC;

    // do the same thing, except copy the arrays over every iteration
    double GPUtime2;
    start_time = std::clock();

    cl::Buffer buffer_A2(context, CL_MEM_READ_WRITE, sizeof(int)*n);
    cl::Buffer buffer_B2(context, CL_MEM_READ_WRITE, sizeof(int)*n);
    cl::Buffer buffer_C2(context, CL_MEM_READ_WRITE, sizeof(int)*n);
    cl::Buffer buffer_constants2(context, CL_MEM_READ_ONLY, sizeof(int)*2);
    for (int i=0; i<k; i++) {
        queue.enqueueWriteBuffer(buffer_A2, CL_TRUE, 0, sizeof(int)*n, A);
        queue.enqueueWriteBuffer(buffer_B2, CL_TRUE, 0, sizeof(int)*n, B);
        queue.enqueueWriteBuffer(buffer_constants2, CL_TRUE, 0, sizeof(int)*2, constants);

        add(buffer_A2, buffer_B2, buffer_C2, buffer_constants2);
    }
    queue.enqueueReadBuffer(buffer_C2, CL_TRUE, 0, sizeof(int)*n, C);
    GPUtime2 = (std::clock() - start_time) / (double) CLOCKS_PER_SEC;

    // let's compare!
    double time_ratio = (CPUtime / GPUtime1);
    std::cout << "VERSION 1 -----------" << std::endl;
    std::cout << "CPU time: " << CPUtime  << std::endl;
    std::cout << "GPU time: " << GPUtime1 << std::endl;
    std::cout << "GPU is ";
    if (time_ratio > 1)
        std::cout << time_ratio << " times faster!" << std::endl;
    else
        std::cout << time_ratio << " times slower :(" << std::endl;

    time_ratio = (CPUtime / GPUtime2);
    std::cout << "\nVERSION 2 -----------" << std::endl;
    std::cout << "CPU time: " << CPUtime  << std::endl;
    std::cout << "GPU time: " << GPUtime2 << std::endl;
    std::cout << "GPU is ";
    if (time_ratio > 1)
        std::cout << time_ratio << " times faster!" << std::endl;
    else
        std::cout << time_ratio << " times slower :(" << std::endl;
    return 0;
}

