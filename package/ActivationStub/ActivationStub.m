/*
 * Orange Pi 5 Plus: the activation answer SpringBoard waits for, without the daemon that cannot start.
 *
 * SpringBoard's main thread asks mobileactivationd for the activation state through NSXPC and waits for the
 * reply with no timeout (-[SBLockdownManager lockdownState] -> -[MadGate getActivationState:], evidence/m7/
 * M7-08). On this port /usr/libexec/mobileactivationd dies by design in its own main() error path (evidence/
 * m7/M7-09), so the reply never comes and the UI never starts.
 *
 * This program is a drop-in for that binary: launchd's com.apple.mobileactivationd job starts it at
 * /usr/libexec/mobileactivationd (the port bind-mounts a payload directory over /usr/libexec), it listens on
 * the job's plain com.apple.mobileactivationd Mach service, and it answers getActivationStateWithCompletionBlock:
 * with @{@"ActivationState": @"Activated"} instead of hanging.
 *
 * Wire facts, all read out of the sealed daemon (/opt/uland/os/usr/libexec/mobileactivationd, 2960496 bytes):
 *   - the selector is -getActivationStateWithCompletionBlock: (strings: "getActivationStateWithCompletionBlock:"
 *     and "-[MobileActivationDaemon getActivationStateWithCompletionBlock:]"); there is no plain -getActivationState:;
 *   - its type encoding is v24@0:8@?<v@?@"NSDictionary"@"NSError">16, i.e. the reply block takes a dictionary
 *     and an error, so the answer is a dictionary and not a bare string;
 *   - the dictionary key is "ActivationState" (_kMAActivationStateKey);
 *   - the state values the daemon knows are "Unactivated", "Activated", "FactoryActivated" and "Unavailable"
 *     (_kMAActivationState{Unactivated,Activated,FactoryActivated,Unavailable});
 *   - the daemon listens on the plain name: "com.apple.mobileactivationd", not on the ".lockdown" name.
 *
 * Deliberately small: only the selector SpringBoard is known to send is implemented, every call is logged
 * to stderr (which the console carries while logd is not the only reader), and an unimplemented selector is
 * answered with an error rather than silence, so the next missing one shows up in the log instead of as a
 * hang.
 */
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <unistd.h>

#define LOG(fmt, ...) fprintf(stderr, "opi5plus: activation-stub: " fmt "\n", ##__VA_ARGS__)

/* One name, both sides: the client connects with -[NSXPCConnection initWithMachServiceName:] on this string
 * (cstring at 0x1d3c921f1 in SpringBoard's caller) and the sealed daemon listens on it (-_main+0x1074). The
 * job also lists com.apple.mobileactivationd.lockdown, which nobody here uses. */
#define MA_SERVICE_NAME "com.apple.mobileactivationd"

/* The client's side is -[MadGate getActivationState:] in MobileActivation.framework; it sends this selector
 * and its reply block reads objectForKeyedSubscript:@"ActivationState". The parameter types here are what
 * produce the daemon's v24@0:8@?<v@?@"NSDictionary"@"NSError">16 on the wire. */
@protocol MadGateProtocol
- (void)getActivationStateWithCompletionBlock:(void (^)(NSDictionary *state, NSError *error))reply;
@end

@interface MadGateService : NSObject <MadGateProtocol>
@end

@implementation MadGateService

- (void)getActivationStateWithCompletionBlock:(void (^)(NSDictionary *, NSError *))reply
{
	/* "Activated" is _kMAActivationStateActivated, the value the sealed daemon reports for an activated
	 * device; of the four known states it is the one that does not send the caller into a different branch
	 * (FactoryActivated is the factory-activation-record path, Unactivated and Unavailable mean "not
	 * activated"). */
	LOG("getActivationStateWithCompletionBlock: -> ActivationState=Activated");
	if (reply != nil) {
		reply(@{@"ActivationState": @"Activated"}, nil);
	}
}

/* Answered rather than ignored: a selector nobody implements is how this path fails next, and an error reply
 * turns that into a log line on the client side instead of another wait with no timeout. */
- (BOOL)respondsToSelector:(SEL)selector
{
	BOOL known = [super respondsToSelector:selector];
	if (!known) {
		LOG("asked for unimplemented selector %s", sel_getName(selector));
	}
	return known;
}

@end

@interface MadGateListener : NSObject <NSXPCListenerDelegate>
@end

@implementation MadGateListener

- (BOOL)listener:(NSXPCListener *)listener shouldAcceptNewConnection:(NSXPCConnection *)connection
{
	LOG("connection from pid %d", (int)connection.processIdentifier);
	NSXPCInterface *interface = [NSXPCInterface interfaceWithProtocol:@protocol(MadGateProtocol)];
	connection.exportedInterface = interface;
	connection.exportedObject = [[MadGateService alloc] init];
	[connection resume];
	return YES;
}

@end

/* -initWithMachServiceName: is API_UNAVAILABLE(ios) in the public SDK, but it is in the iOS Foundation (the
 * selector is in every subcache of this build) and it is what the sealed daemon itself calls on this same job,
 * so declaring it through a protocol keeps the SDK's availability gate out of the way without changing the
 * mechanism: it is still a launchd check-in on a name the job declares, which is what +serviceListener does
 * internally - only now on a name this program picks instead of one launchd picks among two. */
@protocol MAExplicitListenerInit <NSObject>
- (id)initWithMachServiceName:(NSString *)name;
@end

int
main(int argc, char **argv)
{
	/* Written before anything else can fail: this program was never seen running (evidence/m7/M7-10, the
	 * launch-constraint kills), so "did it start at all" has to be readable from the console on the next boot
	 * even if nothing else about it works. */
	fprintf(stderr, "activation-stub: up, service=%s\n", MA_SERVICE_NAME);

	@autoreleasepool {
		(void)argc;
		(void)argv;
		MadGateListener *delegate = [[MadGateListener alloc] init];
		NSString *name = [NSString stringWithUTF8String:MA_SERVICE_NAME];

		NSXPCListener *listener =
		    (NSXPCListener *)[(id<MAExplicitListenerInit>)[NSXPCListener alloc] initWithMachServiceName:name];
		if (listener == nil) {
			/* No explicit listener: fall back to the job's own check-in rather than not serving at all. Which
			 * of the job's two names that lands on is not decidable from here, so it is logged as such. */
			LOG("no listener for %s, falling back to the job's service listener", MA_SERVICE_NAME);
			listener = [NSXPCListener serviceListener];
		}

		if (listener == nil) {
			LOG("no service listener (is the job's MachServices key missing?)");
			return 1;
		}
		listener.delegate = delegate;
		LOG("serving %s at pid %d", MA_SERVICE_NAME, (int)getpid());
		[listener resume];
		/* +serviceListener's resume never returns; an explicitly named listener's does, and main must not
		 * return while the listener is serving. */
		dispatch_main();
	}
	return 0;
}
