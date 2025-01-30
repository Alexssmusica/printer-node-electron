#include <napi.h>
#include <vector>
#include <string>
#include <map>

#ifdef _WIN32
  #include <windows.h>
  #include <winspool.h>
#else
  #include <cups/cups.h>
#endif

#include "print.h"

struct PrinterInfo {
    std::string name;
    bool isDefault;
    std::map<std::string, std::string> options;
    std::string status;
};

std::string GetPrinterStatus(DWORD status) {
    if (status & PRINTER_STATUS_OFFLINE) return "offline";
    if (status & PRINTER_STATUS_ERROR) return "error";
    if (status & PRINTER_STATUS_PAPER_JAM) return "paper-jam";
    if (status & PRINTER_STATUS_PAPER_OUT) return "paper-out";
    if (status & PRINTER_STATUS_MANUAL_FEED) return "manual-feed";
    if (status & PRINTER_STATUS_PAPER_PROBLEM) return "paper-problem";
    if (status & PRINTER_STATUS_BUSY) return "busy";
    if (status & PRINTER_STATUS_PRINTING) return "printing";
    if (status & PRINTER_STATUS_OUTPUT_BIN_FULL) return "output-bin-full";
    if (status & PRINTER_STATUS_NOT_AVAILABLE) return "not-available";
    if (status & PRINTER_STATUS_WAITING) return "waiting";
    if (status & PRINTER_STATUS_PROCESSING) return "processing";
    if (status & PRINTER_STATUS_INITIALIZING) return "initializing";
    if (status & PRINTER_STATUS_WARMING_UP) return "warming-up";
    if (status & PRINTER_STATUS_TONER_LOW) return "toner-low";
    if (status & PRINTER_STATUS_NO_TONER) return "no-toner";
    if (status & PRINTER_STATUS_PAGE_PUNT) return "page-punt";
    if (status & PRINTER_STATUS_USER_INTERVENTION) return "user-intervention";
    if (status & PRINTER_STATUS_OUT_OF_MEMORY) return "out-of-memory";
    if (status & PRINTER_STATUS_DOOR_OPEN) return "door-open";
    return "ready";
}

PrinterInfo GetPrinterDetails(const std::string& printerName, bool isDefault = false) {
    PrinterInfo info;
    info.name = printerName;
    info.isDefault = isDefault;

    HANDLE hPrinter;
    if (OpenPrinter((LPSTR)printerName.c_str(), &hPrinter, NULL)) {
        DWORD needed;
        GetPrinter(hPrinter, 2, NULL, 0, &needed);
        
        if (needed > 0) {
            BYTE* buffer = new BYTE[needed];
            if (GetPrinter(hPrinter, 2, buffer, needed, &needed)) {
                PRINTER_INFO_2* pInfo = (PRINTER_INFO_2*)buffer;
                
                info.status = GetPrinterStatus(pInfo->Status);
                
                // Adicionar opções básicas da impressora
                if (pInfo->pLocation) info.options["location"] = pInfo->pLocation;
                if (pInfo->pComment) info.options["comment"] = pInfo->pComment;
                if (pInfo->pDriverName) info.options["driver"] = pInfo->pDriverName;
                if (pInfo->pPortName) info.options["port"] = pInfo->pPortName;
            }
            delete[] buffer;
        }
        ClosePrinter(hPrinter);
    }

    return info;
}

class GetPrintersWorker : public Napi::AsyncWorker {
private:
    std::vector<PrinterInfo> printers;

public:
    GetPrintersWorker(Napi::Function& callback)
        : Napi::AsyncWorker(callback) {}

    void Execute() override {
        DWORD needed, returned;
        EnumPrinters(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, NULL, 2, NULL, 0, &needed, &returned);
        
        if (needed > 0) {
            BYTE* buffer = new BYTE[needed];
            if (EnumPrinters(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, NULL, 2, buffer, needed, &needed, &returned)) {
                PRINTER_INFO_2* pPrinterEnum = (PRINTER_INFO_2*)buffer;
                
                // Obter impressora padrão para comparação
                char defaultPrinter[256];
                DWORD size = sizeof(defaultPrinter);
                GetDefaultPrinter(defaultPrinter, &size);
                std::string defaultPrinterName(defaultPrinter);

                for (DWORD i = 0; i < returned; i++) {
                    std::string printerName(pPrinterEnum[i].pPrinterName);
                    printers.push_back(GetPrinterDetails(printerName, printerName == defaultPrinterName));
                }
            }
            delete[] buffer;
        }
    }

    void OnOK() override {
        Napi::HandleScope scope(Env());
        Napi::Array printersArray = Napi::Array::New(Env(), printers.size());
        
        for (size_t i = 0; i < printers.size(); i++) {
            Napi::Object printerObj = Napi::Object::New(Env());
            printerObj.Set("name", printers[i].name);
            printerObj.Set("isDefault", printers[i].isDefault);
            printerObj.Set("status", printers[i].status);

            Napi::Object optionsObj = Napi::Object::New(Env());
            for (const auto& option : printers[i].options) {
                optionsObj.Set(option.first, option.second);
            }
            printerObj.Set("options", optionsObj);

            printersArray[i] = printerObj;
        }
        
        Callback().Call({Env().Null(), printersArray});
    }
};

class GetDefaultPrinterWorker : public Napi::AsyncWorker {
private:
    PrinterInfo printer;

public:
    GetDefaultPrinterWorker(Napi::Function& callback)
        : Napi::AsyncWorker(callback) {}

    void Execute() override {
        char defaultPrinter[256];
        DWORD size = sizeof(defaultPrinter);
        
        if (GetDefaultPrinter(defaultPrinter, &size)) {
            printer = GetPrinterDetails(defaultPrinter, true);
        } else {
            SetError("Failed to get default printer");
        }
    }

    void OnOK() override {
        Napi::HandleScope scope(Env());
        
        Napi::Object printerObj = Napi::Object::New(Env());
        printerObj.Set("name", printer.name);
        printerObj.Set("isDefault", printer.isDefault);
        printerObj.Set("status", printer.status);

        Napi::Object optionsObj = Napi::Object::New(Env());
        for (const auto& option : printer.options) {
            optionsObj.Set(option.first, option.second);
        }
        printerObj.Set("options", optionsObj);
        
        Callback().Call({Env().Null(), printerObj});
    }
};

class PrintDirectWorker : public Napi::AsyncWorker {
private:
    std::string printerName;
    std::vector<uint8_t> data;
    std::string dataType;
    std::string result;

public:
    PrintDirectWorker(Napi::Function& callback, std::string printer, std::vector<uint8_t> printData, std::string type = "RAW")
        : Napi::AsyncWorker(callback), printerName(printer), data(printData), dataType(type) {}

    void Execute() override {
        #ifdef _WIN32
        HANDLE printerHandle;
        if (!OpenPrinter((LPSTR)printerName.c_str(), &printerHandle, NULL)) {
            SetError("Failed to open printer");
            return;
        }

        DOC_INFO_1 docInfo;
        docInfo.pDocName = (LPSTR)"Node.js Print Job";
        docInfo.pOutputFile = NULL;
        docInfo.pDatatype = (LPSTR)dataType.c_str();

        if (StartDocPrinter(printerHandle, 1, (LPBYTE)&docInfo)) {
            StartPagePrinter(printerHandle);

            DWORD bytesWritten;
            WritePrinter(printerHandle, data.data(), data.size(), &bytesWritten);
            EndPagePrinter(printerHandle);
            EndDocPrinter(printerHandle);
            result = "Print job created successfully";
        } else {
            SetError("Failed to start document printing");
        }

        ClosePrinter(printerHandle);
        #else
        // ... código CUPS existente ...
        #endif
    }

    void OnOK() override {
        Napi::HandleScope scope(Env());
        Callback().Call({Env().Null(), Napi::String::New(Env(), result)});
    }
};

// Renomear a função PrintText para PrintDirect
Napi::Value PrintDirect(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    std::string printerName;
    std::vector<uint8_t> data;
    std::string dataType = "RAW";  // Valor padrão

    // Verifica se é um objeto de opções
    if (info.Length() == 1 && info[0].IsObject()) {
        Napi::Object options = info[0].As<Napi::Object>();
        
        if (!options.Has("printerName") || !options.Has("data")) {
            Napi::TypeError::New(env, "Object must contain printerName and data properties")
                .ThrowAsJavaScriptException();
            return env.Null();
        }

        printerName = options.Get("printerName").As<Napi::String>().Utf8Value();
        
        // dataType é opcional, verifica se existe
        if (options.Has("dataType") && options.Get("dataType").IsString()) {
            dataType = options.Get("dataType").As<Napi::String>().Utf8Value();
        }

        // Verifica o tipo de data
        Napi::Value dataValue = options.Get("data");
        if (dataValue.IsString()) {
            std::string dataStr = dataValue.As<Napi::String>().Utf8Value();
            data.assign(dataStr.begin(), dataStr.end());
        } else if (dataValue.IsBuffer()) {
            Napi::Buffer<uint8_t> dataBuffer = dataValue.As<Napi::Buffer<uint8_t>>();
            data = std::vector<uint8_t>(dataBuffer.Data(), dataBuffer.Data() + dataBuffer.Length());
        } else {
            Napi::TypeError::New(env, "data must be string or buffer").ThrowAsJavaScriptException();
            return env.Null();
        }
    }
    // Verifica se são argumentos separados
    else if (info.Length() >= 2 && info[0].IsString() && (info[1].IsString() || info[1].IsBuffer())) {
        printerName = info[0].As<Napi::String>().Utf8Value();
        
        // dataType é o terceiro argumento opcional
        if (info.Length() >= 3 && info[2].IsString()) {
            dataType = info[2].As<Napi::String>().Utf8Value();
        }

        if (info[1].IsString()) {
            std::string dataStr = info[1].As<Napi::String>().Utf8Value();
            data.assign(dataStr.begin(), dataStr.end());
        } else {
            Napi::Buffer<uint8_t> dataBuffer = info[1].As<Napi::Buffer<uint8_t>>();
            data = std::vector<uint8_t>(dataBuffer.Data(), dataBuffer.Data() + dataBuffer.Length());
        }
    } else {
        Napi::TypeError::New(env, "Expected either an options object {printerName, data, [dataType]} or at least two arguments: printerName (string), data (string or buffer), [dataType (string)]")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    // Criar uma Promise
    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
    
    // Criar uma função de callback que resolverá a Promise
    Napi::Function callback = Napi::Function::New(env, [deferred](const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        if (info[0].IsNull()) {
            deferred.Resolve(info[1]);
        } else {
            deferred.Reject(info[0].As<Napi::Error>().Value());
        }
        return env.Undefined();
    });
    
    // Criar e iniciar o worker assíncrono
    PrintDirectWorker* worker = new PrintDirectWorker(callback, printerName, data, dataType);
    worker->Queue();
    
    // Retornar a Promise
    return deferred.Promise();
}

Napi::Value GetPrinters(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
    
    Napi::Function callback = Napi::Function::New(env, [deferred](const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        if (info[0].IsNull()) {
            deferred.Resolve(info[1]);
        } else {
            deferred.Reject(info[0].As<Napi::Error>().Value());
        }
        return env.Undefined();
    });
    
    GetPrintersWorker* worker = new GetPrintersWorker(callback);
    worker->Queue();
    
    return deferred.Promise();
}

Napi::Value GetSystemDefaultPrinter(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    // Criar uma Promise
    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
    
    // Criar uma função de callback que resolverá a Promise
    Napi::Function callback = Napi::Function::New(env, [deferred](const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        if (info[0].IsNull()) {
            deferred.Resolve(info[1]);
        } else {
            deferred.Reject(info[0].As<Napi::Error>().Value());
        }
        return env.Undefined();
    });
    
    // Criar e iniciar o worker assíncrono
    GetDefaultPrinterWorker* worker = new GetDefaultPrinterWorker(callback);
    worker->Queue();
    
    // Retornar a Promise
    return deferred.Promise();
}

class GetStatusPrinterWorker : public Napi::AsyncWorker {
private:
    std::string printerName;
    PrinterInfo printer;

public:
    GetStatusPrinterWorker(Napi::Function& callback, std::string name)
        : Napi::AsyncWorker(callback), printerName(name) {}

    void Execute() override {
        printer = GetPrinterDetails(printerName);
        if (printer.name.empty()) {
            SetError("Printer not found");
        }
    }

    void OnOK() override {
        Napi::HandleScope scope(Env());
        
        Napi::Object printerObj = Napi::Object::New(Env());
        printerObj.Set("name", printer.name);
        printerObj.Set("isDefault", printer.isDefault);
        printerObj.Set("status", printer.status);

        Napi::Object optionsObj = Napi::Object::New(Env());
        for (const auto& option : printer.options) {
            optionsObj.Set(option.first, option.second);
        }
        printerObj.Set("options", optionsObj);
        
        Callback().Call({Env().Null(), printerObj});
    }
};

Napi::Value GetStatusPrinter(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    // Verificar se recebemos um argumento
    if (info.Length() < 1 || !info[0].IsObject()) {
        Napi::TypeError::New(env, "Expected an object with printerName property")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    // Obter o objeto de opções
    Napi::Object options = info[0].As<Napi::Object>();
    
    // Verificar se tem a propriedade printerName
    if (!options.Has("printerName") || !options.Get("printerName").IsString()) {
        Napi::TypeError::New(env, "Object must contain printerName as string")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    std::string printerName = options.Get("printerName").As<Napi::String>().Utf8Value();
    
    // Criar uma Promise
    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
    
    // Criar uma função de callback que resolverá a Promise
    Napi::Function callback = Napi::Function::New(env, [deferred](const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        if (info[0].IsNull()) {
            deferred.Resolve(info[1]);
        } else {
            deferred.Reject(info[0].As<Napi::Error>().Value());
        }
        return env.Undefined();
    });
    
    // Criar e iniciar o worker assíncrono
    GetStatusPrinterWorker* worker = new GetStatusPrinterWorker(callback, printerName);
    worker->Queue();
    
    // Retornar a Promise
    return deferred.Promise();
}
