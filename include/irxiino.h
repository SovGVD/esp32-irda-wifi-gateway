#pragma once
#include "xiino.h"
// Capture retained crash breadcrumbs before serving requests.
void irxiino_init();
std::string irxiino_previous_diagnostics();

// Fetch/convert one DataServer request. Local portal pages are handled by irportal.
bool irxiino_page(const XiinoRequest &request, const std::string &headers,
                  std::string &html, std::string &problem);
bool irxiino_stream(const XiinoRequest &request,const std::string &headers,
                    const std::function<bool()> &begin,const XiinoSink &sink,std::string &problem);
std::string irxiino_diagnostics();
