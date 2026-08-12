#ifndef AURORA_SCANNER_SCAN_HANDLER_HPP
#define AURORA_SCANNER_SCAN_HANDLER_HPP

struct ScanJobDesc;
struct UnifiedScanResult;

class IScanHandler {
public:
    virtual ~IScanHandler() = default;

    // 执行具体的扫描任务
    // @param job 扫描任务描述
    // @param result 扫描结果填充
    // @return true 如果扫描成功且产生了有效结果，否则 false
    virtual bool execute(const ScanJobDesc& job, UnifiedScanResult& result) noexcept = 0;
};

#endif // AURORA_SCANNER_SCAN_HANDLER_HPP
