﻿#include <iostream>
#include <string>

#ifdef __linux__
#  include <sys/resource.h>
#  include <systemd/sd-daemon.h>
#elif _WIN32
#  include <fcntl.h>
#  include <io.h>
#  include <Windows.h>
#endif

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/scope_exit.hpp>

#include "vpncore/logging.hpp"
#include "vpncore/tuntap.hpp"
#include "tun2socks/tun2socks.hpp"

using namespace tuntap_service;
using namespace avpncore;

#include "route.hpp"

#ifdef _WIN32
static void setdns()
{
	HKEY dns_key = NULL;
	typedef std::unique_ptr<std::remove_pointer<HKEY>::type,
		decltype(&RegCloseKey)> register_closer;
	DWORD dwDisposition = REG_OPENED_EXISTING_KEY;

	auto status = RegCreateKeyEx(HKEY_LOCAL_MACHINE,
		DNS_KEY, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, 0, &dns_key, &dwDisposition);
	if (status != ERROR_SUCCESS)
		exit(0);
	register_closer adapter_key_close(dns_key, &RegCloseKey);

	const wchar_t* dns_ip = L"127.0.0.1\0";

	status = RegSetValueExW(dns_key, L"DhcpNameServer", 0, REG_SZ, (const BYTE*)dns_ip, lstrlenW(dns_ip) * 2);

	typedef BOOL(WINAPI * DnsFlushResolverCacheFuncPtr)();

	HMODULE dnsapi = LoadLibrary("dnsapi.dll");
	if (dnsapi == NULL) {
		printf("Failed loading module: %d\n", GetLastError());
		return ;
	}

	BOOST_SCOPE_EXIT(&dnsapi)
	{
		FreeLibrary(dnsapi);
	}BOOST_SCOPE_EXIT_END;

	DnsFlushResolverCacheFuncPtr DnsFlushResolverCache = (DnsFlushResolverCacheFuncPtr)GetProcAddress(dnsapi, "DnsFlushResolverCache");
	if (DnsFlushResolverCache == NULL) {
		printf("Failed loading function: %d\n", GetLastError());
		return;
	}
	BOOL result = DnsFlushResolverCache();
	if (result) {
		printf("DnsFlushResolverCache succeeded\n");
	}
	else {
		printf("DnsFlushResolverCache succeeded: %d\n", GetLastError());
	}
}
#endif

int platform_init()
{
#if defined(WIN32) || defined(_WIN32)
	/* Disable the "application crashed" popup. */
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
		SEM_NOOPENFILEERRORBOX);

#if defined(DEBUG) ||defined(_DEBUG)
	//	_CrtDumpMemoryLeaks();
	// 	int flags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
	// 	flags |= _CRTDBG_LEAK_CHECK_DF;
	// 	_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
	// 	_CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDOUT);
	// 	_CrtSetDbgFlag(flags);
#endif

#if !defined(__MINGW32__)
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
#endif

	_setmode(0, _O_BINARY);
	_setmode(1, _O_BINARY);
	_setmode(2, _O_BINARY);

	/* Disable stdio output buffering. */
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	/* Enable minidump when application crashed. */
#elif defined(__linux__)
	rlimit of = { 50000, 100000 };
	if (setrlimit(RLIMIT_NOFILE, &of) < 0)
	{
		perror("setrlimit for nofile");
	}
	struct rlimit core_limit;
	core_limit.rlim_cur = RLIM_INFINITY;
	core_limit.rlim_max = RLIM_INFINITY;
	if (setrlimit(RLIMIT_CORE, &core_limit) < 0)
	{
		perror("setrlimit for coredump");
	}
#endif

	return 0;
}

int main(int argc, char** argv)
{
	platform_init();
	init_logging(false);


	boost::asio::io_context io;

	dev_config cfg = { "10.0.0.1", "255.255.255.0", "10.0.0.0" };
	// dev_config cfg = { "0.0.0.0", "255.255.255.255", "0.0.0.0" };

	cfg.dev_name_ = "VPN01";
	if (argc >= 2)
		cfg.dev_name_ = argv[1];

	tuntap tap(io);
	auto dev_list = tap.take_device_list();
	std::string guid;
	for (auto& i : dev_list)
	{
		if (i.name_ == cfg.dev_name_)
		{
			cfg.guid_ = i.guid_;
			break;
		}
	}

#ifdef AVPN_LINUX
	cfg.dev_name_ = "";
	cfg.guid_ = "";
	cfg.dev_type_ = tuntap_service::dev_tun;
	cfg.tun_fd_ = -1;
#else
	cfg.dev_type_ = tuntap_service::dev_tun;
#endif
	if (!tap.open(cfg))
	{
		LOG_ERR << "open tun device fail!";
		return -1;
	}

	// 创建tun2socks对象.
	tun2socks ts(io, tap);

	// 启动tun2socks.
	ts.start("10.0.0.2", cfg.mask_, argv[2]);

	boost::asio::steady_timer st(io);
	st.expires_after(std::chrono::seconds(0));
	st.async_wait([&tap](auto ec)
	{
		auto index = tap.get_if_index();
		printf("tun device index %d\n", index);
		nl_add_route(tap.get_if_index(), inet_addr("10.0.0.2"));
#ifdef _WIN32
		setdns();
#endif
	});

	// running...
	io.run();

	return 0;
}
