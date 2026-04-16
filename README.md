# Precision-Power

# Dynamic Power Balancer (DPB)
A bare-metal, hyper-optimized Windows background service that dynamically balances CPU and GPU power limits in real-time. Designed specifically for Small Form Factor (SFF) PCs, laptops, and thermally-constrained systems, DPB ensures your system never exceeds its total power supply budget while maximizing gaming and rendering performance.

By reading GPU telemetry via NVIDIA NVML and writing directly to Intel CPU MSRs (Model-Specific Registers) via WinRing0, DPB aggressively clamps CPU package power the moment your GPU spikes and releases it the moment the GPU idles.

# Why This Exists
If you have a 250W power supply, a 150W GPU, and a 125W CPU, running both at 100% load will trip your PSU's Over Current Protection (OCP) and shut down your system. DPB solves this mathematically: Max CPU Power = Total System Budget - Real-Time GPU Power.

Instead of statically crippling your CPU in the BIOS, DPB allows your CPU to boost to its maximum potential during CPU-heavy tasks, seamlessly throttling it back only when the GPU demands the wattage.

# Extreme Optimizations
This program was engineered from the ground up to operate at the absolute limit of the Windows User-Mode ceiling. It consumes effectively 0.00% CPU, < 0.5MB RAM, and 0 disk I/O while running.

The infinite balancing loop: contains zero heap allocations (new, malloc, std::string) and uses strictly integer-based math (no floating-point overhead). Event-Driven OS Hooks drop expensive Ring-0 polling in favor of PowrProf callbacks. Hardware states (Undervolts/Turbo Ratios) are re-applied exactly and only when the NT Kernel broadcasts a Wake-From-Sleep event.

Timer Coalescing: Replaces standard Sleep() with WaitableTimers, allowing the Windows Kernel to group DPB's wake cycles with other background tasks to maintain deep CPU C-States.

Adaptive Polling: Dynamically drops the polling rate from 1,000ms to 5,000ms when the GPU is resting (e.g., < 30W) (these values are all configurable).

Processor Affinity: Supports locking the background loop to specific CPU cores (e.g., E-Cores).

# Configuration
On first launch, DPB generates a config.ini file in its root directory. You can configure:

TOTAL_SYSTEM_BUDGET: The absolute maximum wattage your GPU + CPU can use combined together.

CPU_MAX_WATTS / CPU_MIN_WATTS: Your CPU's dynamic power boundaries.

AFFINITY_MASK: Hex value to pin the process to specific E-Cores.

OFFSET_CPU_CORE_MV: Direct-to-register CPU, Cache, and System Agent undervolting.

ICCMAX_CPU_CORE_A: Unlocking CPU current limits.

# Requirements & Dependencies
OS: Windows 10 / Windows 11 (Requires Administrator privileges / UAC).

GPU: NVIDIA GPU (Relies on nvml.dll included with standard NVIDIA display drivers).

CPU: Intel Processor (Compatible with WinRing0x64.sys).

Dependencies: WinRing0x64.dll and WinRing0x64.sys must be present in the same directory as the executable.

# Building from Source
Compiled using MSVC. For maximum performance, ensure the following linker flags are enabled:

/O2 (Maximize Speed)

/MT (Statically link the C Runtime)

Link against PowrProf.lib for modern sleep state detection.
