/*
    这实际上是5.5版本之后的/lib/kunit/kunit_test_example.c文件的翻译版本
    是我在寻找如何更好地测试内核时发现的，但貌似Kunit不适合我的处理程序的测试。
*/



// SPDX-License-Identifier: GPL-2.0
/*
 * 示例 KUnit 测试，展示如何使用 KUnit。
 *
 * 版权所有 (C) 2019, Google LLC.
 * 作者: Brendan Higgins <brendanhiggins@google.com>
 */

#include <kunit/test.h>
#include <kunit/static_stub.h>

/*
 * 这是 KUnit 最基本的元素——测试用例。一个测试用例会对
 * 某段代码的行为做出一系列期望和断言；如果有任何期望或断言
 * 没有得到满足，测试就失败；否则，测试通过。
 *
 * 在 KUnit 中，一个测试用例就是一个具有以下签名的函数：
 * `void (*)(struct kunit *)`。`struct kunit` 是一个上下文对象，
 * 用于存储当前测试的相关信息。
 */
static void example_simple_test(struct kunit *test)
{
	/*
	 * 这是一个“期望”（EXPECTATION）；它就是 KUnit 用来测试事物的方式。
	 * 当你想测试某段代码时，你会对代码应该做什么设定一些期望。
	 * 然后 KUnit 会运行测试，并验证代码的行为是否与期望相符。
	 */
	KUNIT_EXPECT_EQ(test, 1 + 1, 2);
}

/*
 * 该函数在每个测试用例之前运行一次，更多信息参见
 * example_test_suite 上的注释。
 */
static int example_test_init(struct kunit *test)
{
	kunit_info(test, "正在初始化\n");

	return 0;
}

/*
 * 该函数在每个测试用例之后运行一次，更多信息参见
 * example_test_suite 上的注释。
 */
static void example_test_exit(struct kunit *test)
{
	kunit_info(test, "正在清理\n");
}

/*
 * 该函数在套件中的所有测试用例之前运行一次。
 * 更多信息参见 example_test_suite 上的注释。
 */
static int example_test_init_suite(struct kunit_suite *suite)
{
	kunit_info(suite, "正在初始化套件\n");

	return 0;
}

/*
 * 该函数在套件中的所有测试用例之后运行一次。
 * 更多信息参见 example_test_suite 上的注释。
 */
static void example_test_exit_suite(struct kunit_suite *suite)
{
	kunit_info(suite, "正在退出套件\n");
}

/*
 * 此测试应始终被跳过。
 */
static void example_skip_test(struct kunit *test)
{
	/* 此行应运行 */
	kunit_info(test, "你下面不应该看到一行输出。");

	/* 跳过（并中止）测试 */
	kunit_skip(test, "此测试应被跳过");

	/* 此行不应执行 */
	KUNIT_FAIL(test, "你不应该看到这一行。");
}

/*
 * 此测试应始终被标记为跳过。
 */
static void example_mark_skipped_test(struct kunit *test)
{
	/* 此行应运行 */
	kunit_info(test, "你下面应该看到一行输出。");

	/* 跳过（但不中止）测试 */
	kunit_mark_skipped(test, "此测试应被跳过");

	/* 此行应运行 */
	kunit_info(test, "你应该看到这一行。");
}

/*
 * 此测试展示了所有类型的 KUNIT_EXPECT 宏。
 */
static void example_all_expect_macros_test(struct kunit *test)
{
	const u32 array1[] = { 0x0F, 0xFF };
	const u32 array2[] = { 0x1F, 0xFF };

	/* 布尔断言 */
	KUNIT_EXPECT_TRUE(test, true);
	KUNIT_EXPECT_FALSE(test, false);

	/* 整数断言 */
	KUNIT_EXPECT_EQ(test, 1, 1); /* 检查 == */
	KUNIT_EXPECT_GE(test, 1, 1); /* 检查 >= */
	KUNIT_EXPECT_LE(test, 1, 1); /* 检查 <= */
	KUNIT_EXPECT_NE(test, 1, 0); /* 检查 != */
	KUNIT_EXPECT_GT(test, 1, 0); /* 检查 >  */
	KUNIT_EXPECT_LT(test, 0, 1); /* 检查 <  */

	/* 指针断言 */
	KUNIT_EXPECT_NOT_ERR_OR_NULL(test, test);
	KUNIT_EXPECT_PTR_EQ(test, NULL, NULL);
	KUNIT_EXPECT_PTR_NE(test, test, NULL);
	KUNIT_EXPECT_NULL(test, NULL);
	KUNIT_EXPECT_NOT_NULL(test, test);

	/* 字符串断言 */
	KUNIT_EXPECT_STREQ(test, "hi", "hi");
	KUNIT_EXPECT_STRNEQ(test, "hi", "bye");

	/* 内存块断言 */
	KUNIT_EXPECT_MEMEQ(test, array1, array1, sizeof(array1));
	KUNIT_EXPECT_MEMNEQ(test, array1, array2, sizeof(array1));

	/*
	 * 上述所有宏都有对应的 ASSERT 变体，如果失败则会中止测试执行。
	 * 适用于内存分配等情况。
	 */
	KUNIT_ASSERT_GT(test, sizeof(char), 0);

	/*
	 * 上述所有宏也都有 _MSG 变体，允许在失败时包含附加文本。
	 */
	KUNIT_EXPECT_GT_MSG(test, sizeof(int), 0, "你的 int 是 0 位的吗？！");
	KUNIT_ASSERT_GT_MSG(test, sizeof(int), 0, "你的 int 是 0 位的吗？！");
}

/* 这是我们将用静态桩替换的函数。 */
static int add_one(int i)
{
	/* 如果桩处于激活状态，则会触发。 */
	KUNIT_STATIC_STUB_REDIRECT(add_one, i);

	return i + 1;
}

/* 这是上面函数的替代函数。 */
static int subtract_one(int i)
{
	/* 在替代函数中不需要触发桩。 */

	return i - 1;
}

/*
 * 如果要替换的函数在模块内是静态的，那么导出该函数的指针会很有用，
 * 而不必将静态函数改为非静态导出函数。
 *
 * 此指针模拟模块导出指向静态函数的指针。
 */
static int (* const add_one_fn_ptr)(int i) = add_one;

/*
 * 此测试展示静态桩的使用。
 */
static void example_static_stub_test(struct kunit *test)
{
	/* 默认情况下，函数未被桩化。 */
	KUNIT_EXPECT_EQ(test, add_one(1), 2);

	/* 用 subtract_one() 替换 add_one()。 */
	kunit_activate_static_stub(test, add_one, subtract_one);

	/* add_one() 现在已被替换。 */
	KUNIT_EXPECT_EQ(test, add_one(1), 0);

	/* 将 add_one() 恢复正常。 */
	kunit_deactivate_static_stub(test, add_one);
	KUNIT_EXPECT_EQ(test, add_one(1), 2);
}

/*
 * 此测试展示静态桩在被替换函数以函数指针形式提供时的用法。
 * 当通过导出函数指针来提供对模块中静态函数的访问时，
 * 这很有用，这样就不必将静态函数改为非静态导出函数。
 */
static void example_static_stub_using_fn_ptr_test(struct kunit *test)
{
	/* 默认情况下，函数未被桩化。 */
	KUNIT_EXPECT_EQ(test, add_one(1), 2);

	/* 用 subtract_one() 替换 add_one()。 */
	kunit_activate_static_stub(test, add_one_fn_ptr, subtract_one);

	/* add_one() 现在已被替换。 */
	KUNIT_EXPECT_EQ(test, add_one(1), 0);

	/* 将 add_one() 恢复正常。 */
	kunit_deactivate_static_stub(test, add_one_fn_ptr);
	KUNIT_EXPECT_EQ(test, add_one(1), 2);
}

static const struct example_param {
	int value;
} example_params_array[] = {
	{ .value = 3, },
	{ .value = 2, },
	{ .value = 1, },
	{ .value = 0, },
};

static void example_param_get_desc(const struct example_param *p, char *desc)
{
	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "示例值 %d", p->value);
}

KUNIT_ARRAY_PARAM(example, example_params_array, example_param_get_desc);

/*
 * 此测试展示参数化测试的使用。
 */
static void example_params_test(struct kunit *test)
{
	const struct example_param *param = test->param_value;

	/* 按照设计，参数指针不会为 NULL */
	KUNIT_ASSERT_NOT_NULL(test, param);

	/* 可在不支持的参数值上跳过测试 */
	if (!is_power_of_2(param->value))
		kunit_skip(test, "不支持的参数值 %d", param->value);

	/* 你可以使用参数值进行参数化测试 */
	KUNIT_EXPECT_EQ(test, param->value % param->value, 0);
}

/*
 * 此测试展示 test->priv 的使用。
 */
static void example_priv_test(struct kunit *test)
{
	/* 除非在 suite->init() 中进行了设置，否则 test->priv 为 NULL */
	KUNIT_ASSERT_NULL(test, test->priv);

	/* 但可以用它将任意数据传递给其他函数 */
	test->priv = kunit_kzalloc(test, 1, GFP_KERNEL);
	KUNIT_EXPECT_NOT_NULL(test, test->priv);
	KUNIT_ASSERT_PTR_EQ(test, test->priv, kunit_get_current_test()->priv);
}

/*
 * 此测试应始终通过。可用于练习过滤属性。
 */
static void example_slow_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, 1 + 1, 2);
}

/*
 * 此自定义函数分配内存并设置我们希望存储在
 * kunit_resource->data 字段中的信息。
 */
static int example_resource_init(struct kunit_resource *res, void *context)
{
	int *info = kmalloc(sizeof(*info), GFP_KERNEL);

	if (!info)
		return -ENOMEM;
	*info = *(int *)context;
	res->data = info;
	return 0;
}

/*
 * 此函数释放 kunit_resource->data 字段的内存。
 */
static void example_resource_free(struct kunit_resource *res)
{
	kfree(res->data);
}

/*
 * 此匹配函数由 kunit_find_resource() 调用，
 * 用于根据特定条件定位测试资源。
 */
static bool example_resource_alloc_match(struct kunit *test,
					 struct kunit_resource *res,
					 void *match_data)
{
	return res->data && res->free == example_resource_free;
}

/*
 * 这是一个示例函数，为参数化测试中的每个参数提供描述。
 */
static void example_param_array_get_desc(struct kunit *test, const void *p, char *desc)
{
	const struct example_param *param = p;

	snprintf(desc, KUNIT_PARAM_DESC_SIZE,
		 "示例检查 %d 是否小于或等于 3", param->value);
}

/*
 * 此函数接收参数化测试上下文，即属于参数化测试的 struct kunit。
 * 你可以使用此函数添加想在整个参数化测试中共享的资源，或进行额外的设置。
 */
static int example_param_init(struct kunit *test)
{
	int ctx = 3; /* 待存储的数据。 */
	size_t arr_size = ARRAY_SIZE(example_params_array);

	/*
	 * 此调用分配一个 struct kunit_resource，将其 data 字段设置为 ctx，
	 * 并将其添加到 struct kunit 的资源列表中。请注意，这是由参数化测试管理的。
	 * 因此，它不需要自定义退出函数来进行释放，它会在参数化测试结束时自动清理。
	 */
	void *data = kunit_alloc_resource(test, example_resource_init, example_resource_free,
					  GFP_KERNEL, &ctx);

	if (!data)
		return -ENOMEM;
	/*
	 * 将参数数组信息传递给参数化测试上下文 struct kunit。
	 * 注意，当以这种方式注册参数数组时，需要将 kunit_array_gen_params()
	 * 作为生成器函数提供给 KUNIT_CASE_PARAM_WITH_INIT()。
	 */
	kunit_register_params_array(test, example_params_array, arr_size,
				    example_param_array_get_desc);
	return 0;
}

/*
 * 这是一个使用参数化测试上下文中可用共享资源的示例测试。
 */
static void example_params_test_with_init(struct kunit *test)
{
	int threshold;
	struct kunit_resource *res;
	const struct example_param *param = test->param_value;

	/* 按照设计，参数指针不会为 NULL。 */
	KUNIT_ASSERT_NOT_NULL(test, param);

	/*
	 * 这里我们传递 test->parent 来搜索参数化测试上下文中的共享资源。
	 */
	res = kunit_find_resource(test->parent, example_resource_alloc_match, NULL);

	KUNIT_ASSERT_NOT_NULL(test, res);

	/* 因为 kunit_resource->data 是一个 void 指针，我们需要对其进行类型转换。 */
	threshold = *((int *)res->data);

	/* 断言参数小于或等于某个阈值。 */
	KUNIT_ASSERT_LE(test, param->value, threshold);

	/* 在调用 kunit_find_resource() 后，递减引用计数。 */
	kunit_put_resource(res);
}

/*
 * 辅助函数，用于创建斐波那契数列的参数数组。此示例展示了一种参数生成场景，该场景：
 * 1. 在编译时完全预生成不可行。
 * 2. 使用标准的 generate_params() 函数实现具有挑战性，
 *    因为它只提供前一个参数，而斐波那契数列需要访问前两个值来进行计算。
 */
static void *make_fibonacci_params(struct kunit *test, size_t seq_size)
{
	int *seq;

	if (seq_size <= 0)
		return NULL;
	/*
	 * 这里使用 kunit_kmalloc_array 将数组的生命周期与参数化测试绑定，
	 * 即它将在参数化测试结束后由 KUnit 自动清理。
	 */
	seq = kunit_kmalloc_array(test, seq_size, sizeof(int), GFP_KERNEL);

	if (!seq)
		return NULL;
	if (seq_size >= 1)
		seq[0] = 0;
	if (seq_size >= 2)
		seq[1] = 1;
	for (int i = 2; i < seq_size; i++)
		seq[i] = seq[i - 1] + seq[i - 2];
	return seq;
}

/*
 * 这是一个示例函数，为每个参数提供描述。
 */
static void example_param_dynamic_arr_get_desc(struct kunit *test, const void *p, char *desc)
{
	const int *fib_num = p;

	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "斐波那契参数: %d", *fib_num);
}

/*
 * 参数化测试的 param_init() 函数示例，注册一个动态参数数组。
 */
static int example_param_init_dynamic_arr(struct kunit *test)
{
	size_t seq_size;
	int *fibonacci_params;

	kunit_info(test, "正在初始化参数化测试\n");

	seq_size = 6;
	fibonacci_params = make_fibonacci_params(test, seq_size);

	if (!fibonacci_params)
		return -ENOMEM;

	/*
	 * 将动态参数数组信息传递给参数化测试上下文 struct kunit。
	 * 数组及其元数据将存储在 test->parent->params_array 中。
	 * 数组本身将位于 params_data.params 中。
	 *
	 * 注意，当以这种方式注册参数数组时，需要将 kunit_array_gen_params()
	 * 作为生成器函数提供给 KUNIT_CASE_PARAM_WITH_INIT()。
	 */
	kunit_register_params_array(test, fibonacci_params, seq_size,
				    example_param_dynamic_arr_get_desc);
	return 0;
}

/*
 * 参数化测试的 param_exit() 函数示例，在参数化测试结束时输出日志。
 * 它也可以用于任何其他清理逻辑。
 */
static void example_param_exit_dynamic_arr(struct kunit *test)
{
	kunit_info(test, "正在退出参数化测试\n");
}

/*
 * 使用注册的动态数组执行断言和期望的测试示例。
 */
static void example_params_test_with_init_dynamic_arr(struct kunit *test)
{
	const int *param = test->param_value;
	int param_val;

	/* 按照设计，参数指针不会为 NULL。 */
	KUNIT_ASSERT_NOT_NULL(test, param);

	param_val = *param;
	KUNIT_EXPECT_EQ(test, param_val - param_val, 0);
}

/*
 * 这里我们列出要添加到下面测试套件中的所有测试用例。
 */
static struct kunit_case example_test_cases[] = {
	/*
	 * 这是一个辅助宏，用于从测试用例函数创建测试用例对象；
	 * 它的具体功能对于理解如何使用 KUnit 并不重要，
	 * 只需知道这是将测试用例与测试套件关联起来的方式即可。
	 */
	KUNIT_CASE(example_simple_test),
	KUNIT_CASE(example_skip_test),
	KUNIT_CASE(example_mark_skipped_test),
	KUNIT_CASE(example_all_expect_macros_test),
	KUNIT_CASE(example_static_stub_test),
	KUNIT_CASE(example_static_stub_using_fn_ptr_test),
	KUNIT_CASE(example_priv_test),
	KUNIT_CASE_PARAM(example_params_test, example_gen_params),
	KUNIT_CASE_PARAM_WITH_INIT(example_params_test_with_init, kunit_array_gen_params,
				   example_param_init, NULL),
	KUNIT_CASE_PARAM_WITH_INIT(example_params_test_with_init_dynamic_arr,
				   kunit_array_gen_params, example_param_init_dynamic_arr,
				   example_param_exit_dynamic_arr),
	KUNIT_CASE_SLOW(example_slow_test),
	{}
};

/*
 * 这定义了一个测试套件或一组测试。
 *
 * 测试用例通过将它们添加到 `kunit_cases` 中而被定义为属于该套件。
 *
 * 通常，希望运行某个函数来设置每个测试都会用到的东西；
 * 这可以通过 `init` 函数实现，该函数在每个测试用例被调用之前运行。
 * 类似地，可以指定一个 `exit` 函数，它在每个测试用例之后运行，
 * 可用于清理。为清晰起见，运行测试套件中的测试的行为如下：
 *
 * suite.suite_init(suite);
 * suite.init(test);
 * suite.test_case[0](test);
 * suite.exit(test);
 * suite.init(test);
 * suite.test_case[1](test);
 * suite.exit(test);
 * suite.suite_exit(suite);
 * ...;
 */
static struct kunit_suite example_test_suite = {
	.name = "example",
	.init = example_test_init,
	.exit = example_test_exit,
	.suite_init = example_test_init_suite,
	.suite_exit = example_test_exit_suite,
	.test_cases = example_test_cases,
};

/*
 * 此调用注册上面的测试套件，告诉 KUnit 这是一组需要运行的测试。
 */
kunit_test_suites(&example_test_suite);

static int __init init_add(int x, int y)
{
	return (x + y);
}

/*
 * 此测试应始终通过。可用于测试初始化套件。
 */
static void __init example_init_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, init_add(1, 1), 2);
}

/*
 * kunit_case 结构体不能被标记为 __initdata，因为这将用于
 * 在测试运行后通过 debugfs 获取结果
 */
static struct kunit_case __refdata example_init_test_cases[] = {
	KUNIT_CASE(example_init_test),
	{}
};

/*
 * kunit_suite 结构体不能被标记为 __initdata，因为这将用于
 * 在测试运行后通过 debugfs 获取结果
 */
static struct kunit_suite example_init_test_suite = {
	.name = "example_init",
	.test_cases = example_init_test_cases,
};

/*
 * 此调用注册测试套件，并将该套件标记为使用初始化数据和/或函数。
 */
kunit_test_init_section_suites(&example_init_test_suite);

MODULE_DESCRIPTION("示例 KUnit 测试套件");
MODULE_LICENSE("GPL v2");