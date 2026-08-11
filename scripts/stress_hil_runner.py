#!/usr/bin/env python3
import sys
import time
from hil_runner import run_hil_test

def main():
    iterations = 50
    if len(sys.argv) > 1:
        iterations = int(sys.argv[1])
        
    print(f"Starting HIL Stress Test ({iterations} iterations)...")
    
    passed_count = 0
    failed_count = 0
    
    for i in range(1, iterations + 1):
        print(f"\n==============================================")
        print(f" HIL STRESS TEST: ITERATION {i}/{iterations}")
        print(f"==============================================")
        
        try:
            # We catch SystemExit because run_hil_test() calls sys.exit(1) on failure
            run_hil_test()
            passed_count += 1
            print(f"--> Iteration {i} PASSED.")
        except SystemExit as e:
            if e.code == 0:
                passed_count += 1
                print(f"--> Iteration {i} PASSED.")
            else:
                failed_count += 1
                print(f"--> Iteration {i} FAILED.")
                break # Stop on first failure
        except Exception as e:
            failed_count += 1
            print(f"--> Iteration {i} FAILED with exception: {e}")
            break # Stop on first failure
            
        time.sleep(0.5) # Brief pause between runs
        
    print(f"\n==============================================")
    print(f" STRESS TEST SUMMARY")
    print(f" Total Runs: {i}")
    print(f" Passed:     {passed_count}")
    print(f" Failed:     {failed_count}")
    print(f"==============================================")
    
    if failed_count > 0:
        sys.exit(1)
    else:
        sys.exit(0)

if __name__ == "__main__":
    main()
